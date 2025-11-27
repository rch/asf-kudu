# MemRowSet Flush Crash Analysis

## Date
2025-11-27

## Summary
Segmentation fault in `MemRowSet::Iterator::GetCurrentRow()` during tablet flush operation, likely introduced by commit 802557652 (Nov 25, 2024) which changed `CompactionOrFlushInput` from `unique_ptr` to `shared_ptr`.

## Stack Trace
```
*** SIGSEGV (@0x0) received by PID 1260754
@     0x76bf63d5e079 kudu::tablet::MemRowSet::Iterator::GetCurrentRow()
@     0x76bf63d3c300 kudu::tablet::(anonymous namespace)::MemRowSetCompactionInput::PrepareBlock()
@     0x76bf63d3e4d2 kudu::tablet::FlushCompactionInput()
@     0x76bf63cbdd8c kudu::tablet::Tablet::DoMergeCompactionOrFlush()
@     0x76bf63cc039d kudu::tablet::Tablet::FlushUnlocked()
@     0x76bf63ceed34 kudu::tablet::FlushMRSOp::Perform()
```

## Root Cause Analysis

### Crash Location
File: `src/kudu/tablet/concurrent_btree.h:1699-1703`
```cpp
Slice GetCurrentValue() const {
    DCHECK(seeked_);
    ValueSlice val_slice = leaf_to_scan_->GetValue(idx_in_leaf_);  // <-- CRASH HERE
    return val_slice.as_slice();
}
```

The crash at address `@0x0` indicates `leaf_to_scan_` is either null or points to invalid memory.

### Recent Changes
Commit: `802557652` (Nov 25, 2024)
Title: "[tablet] create CompactionOrFlushInput wrapped into shared_ptr"

Changes in `src/kudu/tablet/memrowset.cc`:
```cpp
// BEFORE:
Status MemRowSet::NewCompactionInput(..., unique_ptr<CompactionOrFlushInput>* out) const {
    out->reset(CompactionOrFlushInput::Create(*this, projection, snap));
    return Status::OK();
}

// AFTER:
Status MemRowSet::NewCompactionInput(..., shared_ptr<CompactionOrFlushInput>* out) const {
    *out = CompactionOrFlushInput::Create(*this, projection, snap);
    return Status::OK();
}
```

### Hypothesis
The `shared_ptr` refactoring may have introduced a subtle lifecycle issue:

1. **MemRowSetCompactionInput** constructor receives `const MemRowSet& memrowset` by reference
2. It calls `memrowset.NewIterator(opts)` which does:
   ```cpp
   return new MemRowSet::Iterator(shared_from_this(), tree_.NewIterator(), opts);
   ```
3. The iterator stores `shared_ptr<const MemRowSet>` via `shared_from_this()`

**Potential Issue**: If the MemRowSet is being destroyed concurrently during flush, or if `shared_from_this()` is called when there's no existing `shared_ptr` to the MemRowSet, the iterator's internal pointers could become invalid.

### Concurrent Access Pattern
The crash occurs during flush with concurrent writes from the CAF event sourcing example. The concurrent B-tree (MassTree-based) has known concurrency edge cases that are difficult to model in TSAN (see `src/kudu/tablet/concurrent_btree.h:38-40`).

## Reproduction
The crash is reproducible under the following conditions:
- Continuous writes to Kudu via CAF/Aeron IPC
- Automatic flush operations triggered by memrowset size
- Multiple concurrent actor instances (5-100 actors)
- Running for ~10-60 seconds

Command:
```bash
cd /home/rch/local/src/rch/asf-kudu
export CHAOS_MODE=CONTINUOUS
export USE_MOCK_BRIDGE=false
export NUM_ACTORS=5
devenv up  # Starts kudu-cluster, kudu-service, manufacturing-sim, caf-example
```

## Proposed Fixes

### Option 1: Add Defensive Null Checks (Band-aid)
```cpp
Slice GetCurrentValue() const {
    DCHECK(seeked_);
    CHECK_NOTNULL(leaf_to_scan_);  // Add null check
    ValueSlice val_slice = leaf_to_scan_->GetValue(idx_in_leaf_);
    return val_slice.as_slice();
}
```

**Pros**: Quick fix, easier to debug
**Cons**: Doesn't address root cause

### Option 2: Verify shared_from_this() Safety
Ensure MemRowSet is always managed by shared_ptr before calling NewCompactionInput:

```cpp
Status MemRowSet::NewCompactionInput(...) const {
    // Verify we're managed by a shared_ptr
    CHECK(shared_from_this().use_count() > 0)
        << "NewCompactionInput called on MemRowSet not managed by shared_ptr";
    *out = CompactionOrFlushInput::Create(*this, projection, snap);
    return Status::OK();
}
```

### Option 3: Hold MemRowSet Reference in CompactionInput
Modify `MemRowSetCompactionInput` to hold a `shared_ptr` to the MemRowSet:

```cpp
class MemRowSetCompactionInput : public CompactionOrFlushInput {
 public:
  MemRowSetCompactionInput(const MemRowSet& memrowset, ...) {
    // Keep MemRowSet alive for duration of compaction
    memrowset_ref_ = memrowset.shared_from_this();
    iter_.reset(memrowset.NewIterator(opts));
  }
 private:
  std::shared_ptr<const MemRowSet> memrowset_ref_;  // ADD THIS
  unique_ptr<MemRowSet::Iterator> iter_;
  // ...
};
```

**Pros**: Guarantees MemRowSet stays alive during compaction
**Cons**: Requires careful review of object lifetimes

### Option 4: Revert commit 802557652
Temporarily revert to unique_ptr until root cause is understood.

## Testing Plan
1. Build with ASAN/TSAN to detect memory issues
2. Run stress test with continuous CAF workload
3. Add unit test for concurrent flush + write scenario
4. Verify with original test case that triggered the crash

## Impact
- **Severity**: High - causes master crash and data unavailability
- **Frequency**: Intermittent under concurrent write/flush load
- **Scope**: Affects any workload with concurrent writes and automatic flushes

## Next Steps
1. Reproduce with ASAN build for better diagnostics
2. Review all call sites of NewCompactionInput for lifecycle issues
3. Coordinate with upstream maintainers (Alexey Serbin, author of 802557652)
4. Consider adding stress test to CI to prevent regression

## References
- Commit 802557652: "[tablet] create CompactionOrFlushInput wrapped into shared_ptr"
- MassTree paper: "Cache Craftiness for Fast Multicore Key-Value Storage" (Eurosys 2012)
- Related file: `src/kudu/tablet/concurrent_btree.h` (concurrent B-tree implementation)

## Status
- [x] Crash reproduced
- [x] Stack trace analyzed
- [x] Recent commits reviewed
- [ ] ASAN build tested
- [ ] Fix implemented
- [ ] Upstream issue filed
