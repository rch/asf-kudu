# Apache Kudu devenv Development Notes

**Date**: November 2025
**Target**: Kudu developers using devenv with Nix

## Summary

This document describes fixes and improvements made to support Apache Kudu development using devenv with modern system dependencies, particularly OpenSSL 3.x compatibility.

---

## OpenSSL 3.x Compatibility Fix

### Status: ✅ MERGED UPSTREAM (KUDU-3716)

**Upstream Commit**: `879a8f9e2` - "KUDU-3716 Add version to IPKI CA CSR"
**Merged**: November 2025
**Resolution**: The fix has been merged from upstream/master and is now part of this branch.

### Original Issue

When building Kudu with OpenSSL 3.4+ (as provided by recent Nix versions), the master server would crash during initialization with the following error:

```
Runtime error: CSR signature verification error: error:05800091:x509 certificate routines::unsupported version:
crypto/x509/x_all.c:47:X509_REQ_verify_ex
```

### Root Cause

OpenSSL 3.4+ became stricter about enforcing RFC 2986 CSR version validation. The Certificate Signing Request (CSR) generation code in `cert_management.cc` did not explicitly set the CSR version, causing validation failures.

### Upstream Solution

The fix was contributed by Attila Bukor and merged into Apache Kudu master branch. The implementation:

**File**: `src/kudu/security/ca/cert_management.cc`
**Lines**: 87-94 in `CertRequestGeneratorBase::GenerateRequest()`

```cpp
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  // Set the request version explicitly to make sure newer OpenSSL versions can
  // handle it.
  //
  // https://github.com/openssl/openssl/pull/24677/
  OPENSSL_RET_NOT_OK(X509_REQ_set_version(req.get(), X509_REQ_VERSION_1),
      "error setting X509 version");
#endif
```

### Compatibility

- ✅ Works with OpenSSL 3.4+ (stricter validation)
- ✅ Works with OpenSSL 3.0-3.3 (existing validation)
- ✅ Works with OpenSSL 1.x (backward compatible via version guard)
- ✅ No behavioral changes for valid CSRs

### Historical Note

This issue was initially discovered and patched locally during devenv development work. The upstream fix uses a more robust approach with OpenSSL version guards (`#if OPENSSL_VERSION_NUMBER >= 0x30000000L`) to ensure compatibility across all OpenSSL versions.

---

## LLVM/Terminfo Configuration Fix (November 2025)

### Issue

After merging upstream/master changes, CMake configuration would fail with:

```
CMake Error at /usr/lib/llvm-14/lib/cmake/llvm/LLVMExports.cmake:68 (set_target_properties):
  The link interface of target "LLVMSupport" contains:

    Terminfo::terminfo

  but the target was not found.
```

### Root Cause

System LLVM 14 was being used instead of Kudu's thirdparty LLVM 11.0.0. System LLVM required terminfo libraries that weren't available in the devenv environment.

### Solution

Explicitly configure CMake to use thirdparty LLVM by setting `-DLLVM_DIR`:

**File**: `devenv.nix` (lines 605-613, 660-668, 632-640, 696-704)

All CMake configuration tasks now include:
```bash
LLVM_CMAKE_DIR="$(pwd)/../../thirdparty/installed/uninstrumented/lib/cmake/llvm"
cmake -DCMAKE_BUILD_TYPE=<type> -DLLVM_DIR="$LLVM_CMAKE_DIR" ../..
```

This ensures Kudu's codegen module uses the bundled LLVM 11.0.0 which has all required dependencies.

---

## DEV_INSTALL_DIR Environment Separation (November 2025)

### Issue

The `DESTDIR` environment variable was polluting thirdparty builds, causing dependencies to be installed to incorrect paths like:
```
/home/user/local/bin/kudu/home/user/src/kudu/thirdparty/installed/uninstrumented/
```

This happened because thirdparty's `make install` commands respected the `DESTDIR` variable from `.env`.

### Root Cause

`DESTDIR` is a standard build system variable that affects **all** `make install` operations:
- Standard behavior: `DESTDIR=/staging` + `PREFIX=/foo` → installs to `/staging/foo`
- Kudu's thirdparty build calls `make install` 27 times
- Setting `DESTDIR` globally polluted all thirdparty installations

### Solution

Introduced `DEV_INSTALL_DIR` as a project-specific variable that only applies to Kudu's final installation:

**Files Modified**:
- `.env` - Changed `DESTDIR` to `DEV_INSTALL_DIR`
- `.env.example` - Updated documentation
- `devenv.nix` - DESTDIR only set at final install point (line 773)

**Configuration** (`.env`):
```bash
# DEV_INSTALL_DIR: Development installation directory for 'make install'
# This is translated to DESTDIR only when running Kudu's 'make install'
# Can be reused across projects (Kudu, Flink, etc.)
DEV_INSTALL_DIR=/home/user/local/bin/kudu
```

**Implementation** (`devenv.nix`, kudu:install task):
```nix
INSTALL_DIR="${DEV_INSTALL_DIR/#\~/$HOME}"
cd build/latest
make install DESTDIR="$INSTALL_DIR"  # Only place DESTDIR is set
```

### Benefits

1. ✅ Thirdparty builds isolated from Kudu installation config
2. ✅ Clean separation without naming conflicts
3. ✅ Reusable across multiple projects (Kudu, Flink, etc.)
4. ✅ No environment pollution

---

## Thirdparty Build Notes

### TSAN Build Issues

When building thirdparty dependencies, TSAN (ThreadSanitizer) builds may fail with:
```
/nix/store/.../ld: cannot find crtbegin.o: No such file or directory
```

This is due to circular dependencies with TSAN runtime libraries. For development work, build only the uninstrumented thirdparty:

```bash
thirdparty/build-thirdparty.sh uninstrumented
```

For release builds, only `common` and `uninstrumented` targets are needed. TSAN builds are only required when specifically testing with ThreadSanitizer.

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
- Starts successfully without segfaults
- Initializes all services (CA, TSK, tablet servers)
- Provides production-like performance
- Still includes debug symbols (via `-g` flag)

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
devenv tasks run kudu:build-debug         # Complete: thirdparty + configure + build

# RELEASE build workflow (recommended)
devenv tasks run kudu:configure-release   # Configure with CMake
devenv tasks run kudu:build-release       # Complete: thirdparty + configure + build

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
devenv tasks run kudu:build-release
ln -sf "$(pwd)/build/release" build/latest
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
devenv tasks run kudu:build-release

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
