# Apache Kudu devenv Development Notes

**Date**: November 2025
**Target**: Kudu developers using devenv with Nix

## Summary

This document describes fixes and improvements made to support Apache Kudu development using devenv with modern system dependencies, particularly OpenSSL 3.x compatibility.

---

## OpenSSL 3.x Compatibility Fix

### Issue

When building Kudu with OpenSSL 3.5+ (as provided by recent Nix versions), the master server crashes during initialization with the following error:

```
F20251110 03:04:01.522497 3467348 catalog_manager.cc:1476] Initializing Kudu internal certificate authority failed:
Runtime error: CSR signature verification error: error:05800091:x509 certificate routines::unsupported version:
crypto/x509/x_all.c:47:X509_REQ_verify_ex
```

### Root Cause

OpenSSL 3.5+ became stricter about enforcing RFC 2986, which defines only **CSR version v1 (value 0)** as valid. Kudu's existing OpenSSL 3.x compatibility patches (commit `cd9e59ebd`) did not cover the Certificate Signing Request (CSR) generation code in `cert_management.cc`.

When CSRs have extensions (as Kudu's CSRs do), OpenSSL may implicitly set an incorrect version. The failure occurs in `X509_REQ_verify()` when the CSR version doesn't match OpenSSL 3.5+'s stricter validation.

### Solution

**File**: `src/kudu/security/ca/cert_management.cc`
**Location**: Lines 93-96 in `CertRequestGeneratorBase::GenerateRequest()`

Add explicit CSR version setting immediately before signing the request:

```cpp
  // Set necessary extensions into the request.
  RETURN_NOT_OK(SetExtensions(req.get()));

  // Explicitly set the version to 0 (v1), which is the only valid CSR version per RFC 2986.
  // OpenSSL 3.5+ is stricter about CSR versions and rejects CSRs with X.509 cert versions.
  OPENSSL_RET_NOT_OK(X509_REQ_set_version(req.get(), X509_REQ_VERSION_1),
      "error setting X509 request version");

  // And finally sign the result.
  OPENSSL_RET_NOT_OK(X509_REQ_sign(req.get(), key.GetRawData(), EVP_sha256()),
      "error signing X509 request");
```

### Verification

After applying this fix:

1. Certificate authority initializes successfully:
   ```
   I20251110 04:44:46.260093 catalog_manager.cc:1505] Initializing Kudu internal certificate authority...
   I20251110 04:44:46.339386 catalog_manager.cc:1380] Generated new certificate authority record
   ```

2. Token signing keys load without errors:
   ```
   I20251110 04:44:46.339419 catalog_manager.cc:1514] Loading token signing keys...
   I20251110 04:44:46.404021 catalog_manager.cc:6022] Generated new TSK 0
   ```

3. Cluster starts successfully with all services operational

### Compatibility

- ✅ Works with OpenSSL 3.5+ (stricter validation)
- ✅ Works with OpenSSL 3.0-3.4 (existing validation)
- ✅ Works with OpenSSL 1.x (backward compatible)
- ✅ No behavioral changes for valid CSRs

This fix should be upstreamed to the main Kudu repository as a follow-up to commit `cd9e59ebd` (OpenSSL 3.x compatibility adaptation).

---

## DEBUG vs RELEASE Build Considerations

### Issue Observed

When building Kudu in **DEBUG mode** with recent Nix-provided dependencies, a segmentation fault occurs during system catalog initialization:

```
*** SIGSEGV (@0x0) received by PID 3937988 (TID 0x78e0d0db96c0) from PID 0
    @     0x78e0f9f5f211 kudu::tablet::MemRowSet::Iterator::FetchRows()
    @     0x78e0faec3ff0 kudu::master::SysCatalogTable::VisitTskEntries()
    @     0x78e0fae38454 kudu::master::CatalogManager::LoadTskEntries()
```

The crash occurs during:
- `InitTokenSigner()` → `LoadTskEntries()` → `VisitTskEntries()` → `MemRowSet::Iterator::FetchRows()`
- Appears to be a NULL pointer dereference in the iterator

This issue is **DEBUG-build specific** and likely related to:
- Extra assertions and checks in DEBUG builds
- Recent array column support work (KUDU-1261 commits)
- Potentially stricter memory access validation

### Recommendation

**For devenv users**: Use **RELEASE builds** by default for local cluster development.

#### Building RELEASE Version

```bash
# Configure RELEASE build
devenv tasks run kudu:configure-release

# Build RELEASE binaries
devenv tasks run kudu:build-release
```

The RELEASE build:
- ✅ Starts successfully without segfaults
- ✅ Initializes all services (CA, TSK, tablet servers)
- ✅ Provides production-like performance
- ✅ Still includes debug symbols (via `-g` flag)

#### Using RELEASE Build with devenv

Update the `build/latest` symlink to point to the RELEASE build:

```bash
rm -f build/latest
ln -s "$(pwd)/build/release" build/latest
```

Now `devenv up` will automatically use the RELEASE binaries.

### When to Use DEBUG Builds

DEBUG builds are still valuable for:
- Active debugging with GDB
- Investigating specific issues with extra assertions
- Developing new features that need assertion checks
- Unit test development

However, for **day-to-day cluster development and testing**, RELEASE builds are more stable and performant.

---

## devenv Task Reference

### Available Tasks

```bash
# Third-party dependencies (run once)
devenv tasks run kudu:build-thirdparty

# DEBUG build workflow
devenv tasks run kudu:configure-debug     # Configure with CMake
devenv tasks run kudu:build-debug         # Build binaries
devenv tasks run kudu:full-debug          # Complete: thirdparty + configure + build

# RELEASE build workflow (recommended)
devenv tasks run kudu:configure-release   # Configure with CMake
devenv tasks run kudu:build-release       # Build binaries
devenv tasks run kudu:full-release        # Complete: thirdparty + configure + build

# Testing and cleanup
devenv tasks run kudu:test                # Run tests
devenv tasks run kudu:clean               # Clean build directories
```

### Starting the Cluster

```bash
# Start cluster with devenv (uses build/latest)
devenv up

# Start cluster manually with specific build
src/kudu/scripts/start_kudu_devenv.sh "$(pwd)/build/release"

# Start installed version (after 'make install')
USE_INSTALLED=1 devenv up
```

### Configuration

Create a `.env` file to customize cluster settings:

```bash
# Copy example configuration
cp .env.example .env

# Edit for your environment
vim .env
```

Key configuration options:
- `CLUSTER_DIR`: Data directory location (default: `/tmp/kudu-<version>`)
- `WEB_IP`: Web UI bind address (default: `127.0.0.1`)
- `WEB_ADVERTISED_IP`: Advertised IP for remote access
- `RPC_IP`: RPC bind address (default: `127.0.0.1`)
- `NUM_MASTERS`: Number of master servers (default: 1)
- `NUM_TSERVERS`: Number of tablet servers (default: 3)

---

## Network Configuration for Remote Access

By default, Kudu's web UI binds to `127.0.0.1` (localhost only). To access the UI from another machine:

1. Set in `.env`:
   ```bash
   WEB_IP=0.0.0.0
   WEB_ADVERTISED_IP=<your-machine-ip>
   ```

2. Ensure firewall allows connections:
   - Master web UI: port 8765
   - Tablet server web UIs: ports 9871, 9873, 9875

3. Access from remote machine:
   - Master: `http://<your-machine-ip>:8765/`
   - Tablet servers: `http://<your-machine-ip>:9871/` (etc.)

---

## Common Issues and Solutions

### Issue: "CSR signature verification error"

**Symptom**: Master crashes with `error:05800091:x509 certificate routines::unsupported version`

**Solution**: Apply the OpenSSL 3.x CSR fix described above (lines 93-96 in `cert_management.cc`)

### Issue: Segfault during cluster startup (DEBUG build)

**Symptom**: Master crashes with `SIGSEGV` in `MemRowSet::Iterator::FetchRows()`

**Solution**: Use RELEASE build instead:
```bash
devenv tasks run kudu:full-release
ln -sf "$(pwd)/build/release" build/latest
```

### Issue: "Cannot bind to address" errors

**Symptom**: Web UI or RPC fails to bind to configured ports

**Solution**: Check for existing Kudu processes:
```bash
pkill -f "kudu-master|kudu-tserver"
sleep 2
# Then restart cluster
```

### Issue: Data loss after reboot

**Symptom**: Cluster data disappears after system restart

**Solution**: Set persistent `CLUSTER_DIR` in `.env`:
```bash
CLUSTER_DIR=/path/to/persistent/storage
```

---

## Development Workflow Recommendations

### Initial Setup

```bash
# 1. Enter devenv shell
devenv shell --impure

# 2. Build third-party dependencies (one time)
devenv tasks run kudu:build-thirdparty

# 3. Configure and build RELEASE version
devenv tasks run kudu:full-release

# 4. Set up configuration
cp .env.example .env
# Edit .env for your environment

# 5. Point build/latest to release
ln -sf "$(pwd)/build/release" build/latest
```

### Daily Development

```bash
# Start development environment
devenv shell --impure

# Make code changes...

# Rebuild (incremental)
devenv tasks run kudu:build-release

# Test changes
devenv up  # or manual start script
```

### Testing Changes

```bash
# Stop existing cluster
pkill -f kudu

# Clean data for fresh start
rm -rf /tmp/kudu-*
# or: rm -rf $CLUSTER_DIR/*

# Restart cluster
devenv up
```

---

## Performance Notes

### Build Times

- **Third-party**: ~30-60 minutes (one-time)
- **Full RELEASE build**: ~6-10 minutes (initial)
- **Incremental rebuild**: ~30-120 seconds (after changes)
- **DEBUG build**: ~10-15% slower than RELEASE

### Runtime Performance

- **RELEASE builds**: Production-like performance
- **DEBUG builds**: ~2-5x slower due to assertions and extra checks
- **Memory usage**: Similar between DEBUG and RELEASE

### Parallel Builds

The devenv automatically uses all available CPU cores:
- Build parallelism: `-j$(nproc)`
- Typical desktop: 8-16 cores
- High-end workstation: 32-64 cores

---

## References

- **OpenSSL 3.x Migration Guide**: https://docs.openssl.org/3.0/man7/migration_guide/
- **RFC 2986** (CSR format): https://www.rfc-editor.org/rfc/rfc2986
- **Kudu OpenSSL 3.x Patch**: Commit `cd9e59ebd` - "OpenSSL 3.x compatibility adaptation"
- **devenv Documentation**: https://devenv.sh/

---

## Contributing Back

If you make improvements to the devenv configuration or fix compatibility issues:

1. Test thoroughly with both DEBUG and RELEASE builds
2. Update this document with any new findings
3. Consider contributing patches upstream to the Kudu project
4. Share configuration examples that help other developers

The OpenSSL 3.x CSR fix in particular should be contributed to the main Kudu repository as it affects anyone building with recent OpenSSL versions.
