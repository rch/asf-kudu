# MemRowSet Flush Crash - Fix Applied

## Date
2025-11-27

## Problem Summary
Segmentation fault in `MemRowSet::Iterator::GetCurrentRow()` during tablet flush, introduced by commit 802557652 which changed `CompactionOrFlushInput` from `unique_ptr` to `shared_ptr`.

## Root Cause
The `MemRowSetCompactionInput` class received a const reference to the MemRowSet, but didn't hold a strong reference to keep it alive during the compaction/flush operation. Under concurrent access (writes + flush), the MemRowSet could be deallocated while the iterator was still accessing its internal B-tree, causing null pointer dereferences.

## Fix Applied

### File: `src/kudu/tablet/compaction.cc`

**Change 1: Add memrowset_ref_ member variable**
```cpp
class MemRowSetCompactionInput : public CompactionOrFlushInput {
 private:
  DISALLOW_COPY_AND_ASSIGN(MemRowSetCompactionInput);

  // Keep MemRowSet alive for the duration of the compaction/flush.
  // This ensures the underlying B-tree and iterator remain valid.
  shared_ptr<const MemRowSet> memrowset_ref_;  // <-- ADDED

  unique_ptr<RowBlock> row_block_;
  unique_ptr<MemRowSet::Iterator> iter_;
  // ...
};
```

**Change 2: Initialize memrowset_ref_ in constructor**
```cpp
MemRowSetCompactionInput(const MemRowSet& memrowset,
                         const MvccSnapshot& snap,
                         const Schema* projection)
  : memrowset_ref_(memrowset.shared_from_this()),  // <-- ADDED
    mem_(32*1024),
    has_more_blocks_(false) {
  RowIteratorOptions opts;
  opts.projection = projection;
  opts.snap_to_include = snap;
  iter_.reset(memrowset.NewIterator(opts));
}
```

## Rationale

The fix ensures that:

1. **Lifetime Guarantee**: The MemRowSet is guaranteed to stay alive for the entire duration of the compaction/flush operation
2. **Thread Safety**: Even if other code releases its references to the MemRowSet, the CompactionInput holds a strong reference
3. **Minimal Change**: Only adds one member variable and one initialization - no changes to existing logic
4. **Symmetric Design**: The Iterator already holds a `shared_ptr<const MemRowSet>` via `shared_from_this()`, so the CompactionInput should too

## Testing

### Build Status
✅ Built successfully:
```bash
cd build/release
make kudu-master kudu-tserver -j8
```

### Recommended Test Procedure
1. **Continuous CAF/Aeron Workload**
   ```bash
   cd /home/rch/local/src/rch/asf-kudu
   export CHAOS_MODE=CONTINUOUS
   export USE_MOCK_BRIDGE=false
   export NUM_ACTORS=100
   export CHAOS_FAILURE_RATE_PER_MIN=20
   devenv up
   ```
   Run for at least 10 minutes. Monitor for crashes.

2. **Stress Test with More Actors**
   ```bash
   export NUM_ACTORS=500
   export METRICS_CONSOLE_INTERVAL_SEC=5
   devenv up
   ```
   Should handle high concurrent write load without crashes.

3. **Monitor Logs**
   Watch for:
   - ✅ No "Segmentation fault" or SIGSEGV messages
   - ✅ Successful flush operations
   - ✅ Continuous metric reports every N seconds
   - ✅ RTO/RPO measurements appearing

### Expected Behavior After Fix
- No crashes during flush operations
- Continuous chaos testing runs indefinitely
- Metrics show successful recoveries with measurable RTO/RPO

## Files Modified
- `src/kudu/tablet/compaction.cc` (2 changes)

## Files Created (Documentation)
- `docs/scratch/2025-11-27/01_memrowset_flush_crash_analysis.md`
- `docs/scratch/2025-11-27/02_memrowset_fix_applied.md`

## Upstream Communication
This fix should be:
1. Tested thoroughly with the CAF example
2. Documented with reproduction steps
3. Submitted as a patch to Apache Kudu
4. Referenced in any PR that includes the CAF/Aeron example

Suggested commit message:
```
[tablet] Fix MemRowSet lifecycle issue in flush

Commit 802557652 changed CompactionOrFlushInput to use shared_ptr,
but MemRowSetCompactionInput didn't hold a strong reference to the
MemRowSet. Under concurrent access, the MemRowSet could be destroyed
while flush was in progress, causing crashes in the B-tree iterator.

This patch adds a shared_ptr member to MemRowSetCompactionInput to
guarantee the MemRowSet stays alive for the duration of the flush.

Fixes crash: SIGSEGV in MemRowSet::Iterator::GetCurrentRow()
Discovered by: CAF/Aeron event sourcing example under chaos testing
```

## Next Steps
1. [ ] Test fix with continuous chaos workload
2. [ ] Verify request/response correlation works correctly
3. [ ] Measure RTO/RPO metrics under failure injection
4. [ ] Document findings for upstream Kudu team
5. [ ] Complete CAF/Aeron example documentation

## Related Work
- Request ID truncation bug (fixed in `examples/caf/aeron_ipc_bridge.cpp`)
- Continuous chaos testing framework (implemented)
- RTO/RPO metrics collection (implemented)
