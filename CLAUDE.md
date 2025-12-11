# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Apache Kudu is a columnar storage system for the Apache Hadoop ecosystem. It's a hybrid between HDFS and HBase, designed for fast analytics on fast data. The codebase is primarily C++ with Java client libraries and tooling.

## Build System

Kudu uses CMake for C++ builds and Gradle for Java builds. The build system supports out-of-tree builds with a multi-build layout.

### C++ Build Commands

```bash
# Configure and build (from build directory)
mkdir -p build/debug
cd build/debug
cmake ../..
make -j8

# Build types: debug, fastdebug, release, profile_gen, profile_build
# Example for release build:
mkdir -p build/release
cd build/release
cmake -DCMAKE_BUILD_TYPE=release ../..
make -j8

# Run all tests
ctest -j8

# Run a specific test
build/debug/bin/tablet-test --gtest_filter=TestTablet/9.TestFlush

# List tests in a binary
build/debug/bin/tablet-test --gtest_list_tests
```

**Important CMake flags:**
- `-DNO_TESTS=1` - Skip building tests
- `-DKUDU_USE_ASAN=1` - Enable AddressSanitizer
- `-DKUDU_USE_TSAN=1` - Enable ThreadSanitizer
- `-DKUDU_USE_UBSAN=1` - Enable UndefinedBehaviorSanitizer
- `-DKUDU_LINK=dynamic|static` - Link type (auto by default)
- `-DKUDU_GENERATE_COVERAGE=1` - Generate code coverage

### Java Build Commands

```bash
cd java

# Build everything
./gradlew assemble

# Build just the client
./gradlew :kudu-client:assemble

# Run all tests
./gradlew test

# Run specific test class
./gradlew :kudu-client:test --tests org.apache.kudu.TestColumnSchema

# Run specific test method
./gradlew :kudu-client:test --tests org.apache.kudu.TestColumnSchema.testEquals

# Show test output
./gradlew -DshowTestOutput :kudu-client:test

# Install to local Maven repo
./gradlew publishToMavenLocal
```

Java tests expect C++ binaries in `build/latest/bin`. Use `-DkuduBinDir=/path/to/dir` to override.

### Code Quality Tools

```bash
# Lint checks (incremental - only changed files)
make ilint

# Lint checks (full scan)
make lint

# Clang-tidy checks
make tidy

# Include-what-you-use checks
make iwyu
./build-support/iwyu.py  # For specific files or full scan

# Generate tags
make ctags  # or: make etags, make cscope
```

## Architecture

### Core Components

**C++ Source Structure (`src/kudu/`):**
- `master/` - Master server: manages metadata, tablet placement, cluster coordination
- `tserver/` - Tablet server: serves data, executes reads/writes
- `tablet/` - Tablet implementation: storage engine, write-ahead log, compaction
- `consensus/` - Raft consensus implementation for replication
- `rpc/` - RPC framework (KRPC - Kudu RPC)
- `client/` - C++ client library
- `common/` - Common data structures, schema definitions
- `cfile/` - CFile format: columnar storage file format
- `fs/` - File system abstractions and block management
- `util/` - Utility libraries (threading, logging, metrics, etc.)
- `security/` - Authentication (Kerberos, tokens), TLS, authorization
- `tools/` - Command-line tools (`kudu` CLI)

**Java Source Structure (`java/`):**
- `kudu-client/` - Java client library
- `kudu-spark/` - Spark integration
- `kudu-hive/` - Hive Metastore integration
- `kudu-backup/` - Backup and restore tools
- `kudu-subprocess/` - Subprocess server for Ranger authorization

### Key Architectural Patterns

**Storage Layer:**
- Tablets are the unit of horizontal partitioning (like HBase regions)
- Each tablet has a MemRowSet (in-memory) and multiple DiskRowSets (on-disk)
- Columnar storage using CFile format with compression and encoding
- Block manager handles physical storage (file or log block manager)

**Replication:**
- Raft consensus for strong consistency
- Each tablet has a Raft group (leader + followers)
- Write-ahead log (WAL) for durability
- Leader handles writes, followers can serve reads (with snapshot consistency)

**RPC System:**
- KRPC: custom RPC framework built on protobuf and libev
- Service definitions in `.proto` files
- Code generation via `pb-gen`, `krpc-gen` targets

**Security:**
- Internal PKI: self-signed CA, automatic certificate distribution
- Token-based authentication for internal RPCs
- Kerberos support for external authentication
- TLS for all network communication

### Third-Party Dependencies

Dependencies are managed in `thirdparty/`. The build system automatically rebuilds them if needed via `thirdparty/build-if-necessary.sh`.

To disable automatic rebuild: `NO_REBUILD_THIRDPARTY=1 cmake ../..`

Major dependencies: glog, gflags, protobuf, boost, rocksdb, OpenSSL, Kerberos

## Development Workflow

### Setting Up a Development Environment

The project supports devenv (Nix-based) for reproducible development environments. See `DEVENV_NOTES.md` for details on the devenv setup, including OpenSSL 3.x compatibility fixes.

**Traditional setup:**
1. Install dependencies (see README.adoc)
2. Build third-party: `cd thirdparty && ./build-if-necessary.sh`
3. Configure: `mkdir -p build/debug && cd build/debug && cmake ../..`
4. Build: `make -j8`
5. Run tests: `ctest -j8`

### Testing

**C++ Tests:**
- Use Google Test framework
- Test binaries in `build/<type>/bin/`
- Controlled by `build-support/run-test.sh` wrapper
- Test logs in `build/<type>/test-logs/`
- Sharded tests split across multiple processes for parallelism

**Java Tests:**
- Use JUnit
- Require C++ binaries for integration tests
- Managed by Gradle

**Important Environment Variables:**
- `KUDU_TEST_TIMEOUT` - Test timeout in seconds
- `KUDU_COMPRESS_TEST_OUTPUT` - Compress test logs
- `KUDU_FLAKY_TEST_ATTEMPTS` - Retry flaky tests
- `KUDU_ALLOW_SLOW_TESTS` - Enable slow tests

### Adding New Features

**For C++ code:**
1. Add source files to appropriate `CMakeLists.txt`
2. Use `ADD_KUDU_TEST()` for test binaries
3. Follow coding style (checked by cpplint)
4. Add unit tests in `*-test.cc` files

**For protobuf changes:**
1. Edit `.proto` files
2. Regenerate: `make pb-gen` or `make krpc-gen`
3. Generated files go to `build/<type>/src/`

**For new RPC services:**
1. Define service in `.proto` file with `service` keyword
2. Implement service handler inheriting from generated base class
3. Register service with RPC server

### Code Generation

Generated code targets:
- `make pb-gen` - Protobuf code
- `make krpc-gen` - RPC service code
- `make fb-gen` - Flatbuffers code
- `make generated-headers` - All generated headers (includes HMS thrift)

## Important Notes

**Build Directory Layout:**
- Never build in the source root (enforced by CMake)
- Use `build/<build-type>/` structure
- `build/latest` symlink points to most recent build
- Build artifacts: `lib/`, `bin/`, `test-logs/`

**Linking:**
- Debug/FastDebug builds use dynamic linking by default (faster iteration)
- Release builds use static linking by default
- TSAN builds require dynamic linking
- Can override with `-DKUDU_LINK=static|dynamic`

**Sanitizers:**
- ASAN/TSAN/UBSAN require clang (use `thirdparty/clang-toolchain/bin/clang`)
- TSAN requires special libstdc++ in `thirdparty/installed/tsan/`
- Cannot use ASAN and TSAN simultaneously
- Sanitizer blacklist: `build-support/sanitize-blacklist.txt`

**Performance:**
- Use ccache for faster rebuilds (add `/usr/lib/ccache` to PATH)
- Use gold linker instead of GNU ld for faster linking
- Consider LTO builds for maximum performance (`-DKUDU_USE_LTO=1` with clang+lld)

**File Locations:**
- Source: `src/kudu/`
- Tests: `src/kudu/*/` (files named `*-test.cc`)
- Tools: `src/kudu/tools/`
- Build support scripts: `build-support/`
- Third-party: `thirdparty/`
- Documentation: `docs/`

## Common Patterns

**Error Handling:**
- Use `Status` class for error returns
- Macros: `RETURN_NOT_OK()`, `WARN_NOT_OK()`, `CHECK_OK()`
- For OpenSSL: `OPENSSL_RET_NOT_OK()`, `OPENSSL_CHECK_OK()`

**Logging:**
- glog-based: `LOG(INFO)`, `LOG(WARNING)`, `LOG(ERROR)`, `LOG(FATAL)`
- `VLOG(level)` for verbose logging
- `CHECK()` for assertions

**Threading:**
- Use `kudu::Thread` instead of raw pthreads
- Thread pools via `ThreadPool` class
- Synchronization: `Mutex`, `RWMutex`, `Semaphore`

**Memory Management:**
- Smart pointers: `std::unique_ptr`, `std::shared_ptr`
- Reference counting: `scoped_refptr<T>` for RefCountedThreadSafe classes
- Arena allocators for short-lived allocations

## Known Issues

**OpenSSL 3.5+ Compatibility:**
- CSR generation requires explicit version setting (see DEVENV_NOTES.md)
- Fix applied in `src/kudu/security/ca/cert_management.cc`

**Debug Build Stability:**
- Some debug builds may have issues with recent dependencies
- Use release builds for stable development clusters
- Debug builds are ~2-5x slower due to assertions

## Additional Resources

- Main documentation: `docs/` directory
- README: `README.adoc`
- Contributing guide: `CONTRIBUTING.adoc`
- Release process: `RELEASING.adoc`
- Java-specific docs: `java/README.adoc`
- devenv setup: `DEVENV_NOTES.md`
