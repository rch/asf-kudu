{ pkgs, lib, config, inputs, ... }:

{
  dotenv.enable = true;

  # Long-running processes managed by 'devenv up'
  processes = {
    # Aeron Media Driver (IPC transport for CAF examples)
    aeron-driver.exec = ''
      if [ "''${ENABLE_CAF_EXAMPLE:-false}" != "true" ] && [ "''${ENABLE_MANUFACTURING_SIM:-false}" != "true" ]; then
        echo "Aeron Media Driver disabled (enable CAF examples to start)"
        sleep infinity
      fi

      echo "╔══════════════════════════════════════════════════════════════╗"
      echo "║  AERON MEDIA DRIVER - Ultra-Low-Latency IPC Transport       ║"
      echo "╚══════════════════════════════════════════════════════════════╝"
      echo ""

      # Clean up stale Aeron directories from previous runs
      AERON_DIR="/dev/shm/aeron-$(whoami)"
      if [ -d "$AERON_DIR" ]; then
        echo "Cleaning up stale Aeron directory: $AERON_DIR"
        rm -rf "$AERON_DIR"
      fi

      echo "Starting Aeron Media Driver..."
      echo "  IPC Channel: aeron:ipc"
      echo "  Shared Memory: $AERON_DIR"
      echo "  Thread Model: Dedicated (ultra-low latency)"
      echo ""
      echo "Monitor with:"
      echo "  ls -lh $AERON_DIR"
      echo "  cat $AERON_DIR/cnc-stats"
      echo ""
      echo "══════════════════════════════════════════════════════════════"
      echo ""

      # Run Aeron media driver (C++ version from devenv packages)
      # The driver runs until killed, managing IPC channels in /dev/shm
      exec aeronmd
    '';

    # MIT Kerberos KDC (optional - enable with ENABLE_KERBEROS=true)
    kerberos-kdc.exec = ''
      if [ "''${ENABLE_KERBEROS:-false}" != "true" ]; then
        echo "Kerberos KDC disabled (set ENABLE_KERBEROS=true in .env to enable)"
        sleep infinity
      fi

      # Initialize Kerberos if needed
      if [ ! -f .devenv/kerberos/principal ]; then
        echo "Initializing Kerberos KDC..."
        ./scripts/kerberos/init-kerberos.sh
      fi

      # Start KDC
      echo "Starting Kerberos KDC (port 10088)..."
      echo "  Realm: ''${FREEIPA_REALM:-VISTA.ZNDX.ORG}"
      echo "  Config: .devenv/kerberos/kdc.conf"
      echo ""
      export KRB5_CONFIG=.devenv/kerberos/krb5.conf
      export KRB5_KDC_PROFILE=.devenv/kerberos/kdc.conf
      exec ${pkgs.krb5Full}/bin/krb5kdc -n
    '';

    # BIND9 DNS Server (optional - enable with ENABLE_BIND_DNS=true)
    bind-dns.exec = ''
      if [ "''${ENABLE_BIND_DNS:-false}" != "true" ]; then
        echo "BIND DNS disabled (set ENABLE_BIND_DNS=true in .env to enable)"
        sleep infinity
      fi

      # Initialize BIND if needed
      if [ ! -f .devenv/bind/named.conf ]; then
        echo "Initializing BIND DNS..."
        ./scripts/bind/init-bind.sh
      fi

      # Start BIND
      echo "Starting BIND DNS server (port 5353)..."
      echo "  Domain: ''${FREEIPA_DOMAIN:-vista.zndx.lan}"
      echo "  Config: .devenv/bind/named.conf"
      echo ""
      echo "Query with: dig @127.0.0.1 -p 5353 <hostname>"
      echo ""
      exec ${pkgs.bind}/bin/named -f -c .devenv/bind/named.conf -p 5353
    '';

    # Kudu Cluster
    kudu-cluster.exec = ''
      # Determine which directory to use: build or install
      DIR_ARG=""
      if [ "$USE_INSTALLED" = "1" ] || [ "$USE_INSTALLED" = "true" ]; then
        # User explicitly requested installed version
        if [ -n "$DEV_INSTALL_DIR" ]; then
          INSTALL_DIR="''${DEV_INSTALL_DIR/#\~/$HOME}"
          if [ -x "$INSTALL_DIR/usr/local/sbin/kudu-master" ]; then
            DIR_ARG="$INSTALL_DIR/usr/local"
          else
            echo "Error: USE_INSTALLED=1 but no installation found at $INSTALL_DIR"
            echo "Run 'devenv tasks run kudu:install' first."
            exit 1
          fi
        else
          echo "Error: USE_INSTALLED=1 but DEV_INSTALL_DIR not set in .env"
          exit 1
        fi
      elif [ -L build/latest ] && [ -x build/latest/bin/kudu-master ]; then
        # Use build directory (preferred)
        DIR_ARG="$(cd build/latest && pwd)"
      elif [ -n "$DEV_INSTALL_DIR" ]; then
        # Fall back to installation
        INSTALL_DIR="''${DEV_INSTALL_DIR/#\~/$HOME}"
        if [ -x "$INSTALL_DIR/usr/local/sbin/kudu-master" ]; then
          DIR_ARG="$INSTALL_DIR/usr/local"
        fi
      fi

      if [ -z "$DIR_ARG" ]; then
        echo "Error: No Kudu build or installation found."
        echo "Run 'devenv tasks run kudu:build-debug' first."
        exit 1
      fi

      # Call the devenv-specific wrapper script
      exec src/kudu/scripts/start_kudu_devenv.sh "$DIR_ARG"
    '';

    # Kudu Service (Aeron + Kudu bridge for CAF examples)
    kudu-service.exec = ''
      if [ "''${ENABLE_CAF_EXAMPLE:-false}" != "true" ] && [ "''${ENABLE_MANUFACTURING_SIM:-false}" != "true" ]; then
        echo "Kudu Service disabled (enable CAF examples to start)"
        sleep infinity
      fi

      echo "╔══════════════════════════════════════════════════════════════╗"
      echo "║  KUDU SERVICE - Aeron IPC to Kudu Bridge                    ║"
      echo "╚══════════════════════════════════════════════════════════════╝"
      echo ""
      echo "Waiting for Kudu cluster to be ready..."

      # Wait for Kudu cluster to be fully started (check master port)
      MASTER_ADDR="''${RPC_IP:-127.0.0.1}:8764"
      MAX_WAIT=60
      ELAPSED=0

      while [ $ELAPSED -lt $MAX_WAIT ]; do
        if nc -z ''${RPC_IP:-127.0.0.1} 8764 2>/dev/null; then
          echo "✓ Kudu master is listening on port 8764"
          break
        fi
        sleep 1
        ELAPSED=$((ELAPSED + 1))
        if [ $((ELAPSED % 10)) -eq 0 ]; then
          echo "  Still waiting for Kudu master... ($ELAPSED seconds)"
        fi
      done

      if [ $ELAPSED -ge $MAX_WAIT ]; then
        echo "✗ Timeout waiting for Kudu cluster to start"
        echo "  Make sure the kudu-cluster process is running"
        sleep infinity
        exit 1
      fi

      # Give Kudu cluster a bit more time to fully initialize
      echo "Waiting for Kudu cluster to fully initialize..."
      sleep 3

      # Check if executable exists
      if [ ! -x examples/caf/build/kudu_service_simple ]; then
        echo "Building kudu_service_simple..."
        mkdir -p examples/caf/build
        cd examples/caf/build

        # Use thirdparty cmake if available, otherwise system cmake
        if [ -f ../../../thirdparty/installed/common/bin/cmake ]; then
          ../../../thirdparty/installed/common/bin/cmake .. 2>&1 | tail -10
        else
          cmake .. 2>&1 | tail -10
        fi

        if ! make -j$(nproc) kudu_service_simple 2>&1 | tail -10; then
          echo ""
          echo "✗ Build failed"
          echo "To see full build output, run:"
          echo "  cd examples/caf/build && cmake .. && make kudu_service_simple"
          sleep infinity
          exit 1
        fi
        cd ../../..
      fi

      echo "✓ Build complete"
      echo ""

      # Wait for Aeron driver to be ready
      echo "Waiting for Aeron media driver..."
      AERON_DIR="/dev/shm/aeron-$(whoami)"
      MAX_WAIT=30
      ELAPSED=0

      while [ $ELAPSED -lt $MAX_WAIT ]; do
        if [ -d "$AERON_DIR" ] && [ -f "$AERON_DIR/cnc.dat" ]; then
          echo "✓ Aeron media driver is ready"
          break
        fi
        sleep 1
        ELAPSED=$((ELAPSED + 1))
        if [ $((ELAPSED % 5)) -eq 0 ]; then
          echo "  Still waiting for Aeron... ($ELAPSED seconds)"
        fi
      done

      if [ $ELAPSED -ge $MAX_WAIT ]; then
        echo "✗ Timeout waiting for Aeron media driver"
        echo "  Make sure the aeron-driver process is running"
        sleep infinity
        exit 1
      fi

      echo ""
      echo "Starting Kudu Service..."
      echo "  Master addresses: ''${RPC_IP:-127.0.0.1}:8764"
      echo "  Aeron IPC channel: aeron:ipc"
      echo "  Stream ID: 1001"
      echo ""
      echo "Architecture:"
      echo "  CAF Actors → Aeron IPC → Kudu Service → Kudu Cluster"
      echo ""
      echo "══════════════════════════════════════════════════════════════"
      echo ""

      # Run the service (pass master address as positional argument)
      ./examples/caf/build/kudu_service_simple ''${RPC_IP:-127.0.0.1}:8764
      EXIT_CODE=$?

      if [ $EXIT_CODE -eq 0 ]; then
        echo ""
        echo "✓ Kudu Service exited cleanly"
      else
        echo ""
        echo "✗ Kudu Service failed with exit code $EXIT_CODE"
      fi

      sleep infinity
    '';

    # CAF Event Sourcing Example (optional - enable with ENABLE_CAF_EXAMPLE=true)
    caf-example.exec = ''
      if [ "''${ENABLE_CAF_EXAMPLE:-false}" != "true" ]; then
        echo "CAF Event Sourcing Example disabled (set ENABLE_CAF_EXAMPLE=true to enable)"
        sleep infinity
      fi

      echo "╔══════════════════════════════════════════════════════════════╗"
      echo "║  CONTINUOUS CHAOS TESTING - EVENT SOURCING VALIDATION        ║"
      echo "╚══════════════════════════════════════════════════════════════╝"
      echo ""

      # Wait for Aeron driver to be ready
      echo "Waiting for Aeron media driver..."
      AERON_DIR="/dev/shm/aeron-$(whoami)"
      MAX_WAIT=30
      ELAPSED=0

      while [ $ELAPSED -lt $MAX_WAIT ]; do
        if [ -d "$AERON_DIR" ] && [ -f "$AERON_DIR/cnc.dat" ]; then
          echo "✓ Aeron media driver is ready"
          break
        fi
        sleep 1
        ELAPSED=$((ELAPSED + 1))
        if [ $((ELAPSED % 5)) -eq 0 ]; then
          echo "  Still waiting for Aeron... ($ELAPSED seconds)"
        fi
      done

      if [ $ELAPSED -ge $MAX_WAIT ]; then
        echo "✗ Timeout waiting for Aeron media driver"
        echo "  Make sure the aeron-driver process is running"
        sleep infinity
        exit 1
      fi

      # Check if executable exists
      if [ ! -x examples/caf/build/test_event_sourcing ]; then
        echo "Building test_event_sourcing..."
        mkdir -p examples/caf/build
        cd examples/caf/build

        # Use thirdparty cmake if available, otherwise system cmake
        if [ -f ../../../thirdparty/installed/common/bin/cmake ]; then
          ../../../thirdparty/installed/common/bin/cmake .. 2>&1 | tail -10
        else
          cmake .. 2>&1 | tail -10
        fi

        if ! make -j$(nproc) test_event_sourcing 2>&1 | tail -10; then
          echo ""
          echo "✗ Build failed"
          echo "To see full build output, run:"
          echo "  cd examples/caf/build && cmake .. && make test_event_sourcing"
          sleep infinity
          exit 1
        fi
        cd ../../..
      fi

      echo "✓ Build complete"
      echo ""

      # Chaos testing configuration (from environment or defaults)
      export CHAOS_MODE="''${CHAOS_MODE:-CONTINUOUS}"
      export USE_MOCK_BRIDGE="''${USE_MOCK_BRIDGE:-false}"
      export NUM_ACTORS="''${NUM_ACTORS:-100}"
      export CHAOS_FAILURE_RATE_PER_MIN="''${CHAOS_FAILURE_RATE_PER_MIN:-10}"
      export CHAOS_DELAY_PROBABILITY="''${CHAOS_DELAY_PROBABILITY:-15}"
      export METRICS_CONSOLE_INTERVAL_SEC="''${METRICS_CONSOLE_INTERVAL_SEC:-10}"

      echo "Chaos Testing Configuration:"
      echo "  Mode:                   $CHAOS_MODE"
      echo "  Bridge:                 $([ "$USE_MOCK_BRIDGE" = "true" ] && echo "MOCK" || echo "REAL (Aeron IPC)")"
      echo "  Actors:                 $NUM_ACTORS"
      echo "  Failures per minute:    $CHAOS_FAILURE_RATE_PER_MIN"
      echo "  Message delay prob:     $CHAOS_DELAY_PROBABILITY%"
      echo "  Metrics interval:       $METRICS_CONSOLE_INTERVAL_SEC seconds"
      echo "══════════════════════════════════════════════════════════════"
      echo ""
      echo "Running continuous chaos testing..."
      echo "  Press Ctrl+C to stop"
      echo "  Monitor with: top -p \$(pgrep test_event_sourcing)"
      echo ""

      # Run continuously with chaos testing
      # The test will loop internally - no need for shell loop
      ./examples/caf/build/test_event_sourcing

      EXIT_CODE=$?
      echo ""
      if [ $EXIT_CODE -eq 0 ]; then
        echo "✓ Chaos Test exited cleanly (exit code: 0)"
      elif [ $EXIT_CODE -eq 130 ]; then
        echo "✓ Chaos Test stopped by user (Ctrl+C)"
      else
        echo "✗ Chaos Test failed with exit code $EXIT_CODE"
      fi

      # Keep process alive so devenv doesn't restart it
      echo "Chaos Test: Sleeping (process will restart if devenv reloads)..."
      sleep infinity
    '';

    # Manufacturing Event Generator - Continuous simulation for profiling
    manufacturing-sim = {
      exec = ''
        # Only run if enabled
        if [ "''${ENABLE_MANUFACTURING_SIM:-false}" != "true" ]; then
          echo "Manufacturing Simulator disabled (set ENABLE_MANUFACTURING_SIM=true to enable)"
          sleep infinity
        fi

        echo "╔══════════════════════════════════════════════════════════════╗"
        echo "║  MANUFACTURING FACILITY SIMULATOR                            ║"
        echo "╚══════════════════════════════════════════════════════════════╝"
        echo ""

        # Check if executable exists
        if [ ! -x examples/caf/build/manufacturing_event_generator ]; then
          echo "Building manufacturing_event_generator..."
          mkdir -p examples/caf/build
          cd examples/caf/build

          # Use thirdparty cmake if available, otherwise system cmake
          if [ -f ../../../thirdparty/installed/common/bin/cmake ]; then
            ../../../thirdparty/installed/common/bin/cmake .. 2>&1 | tail -10
          else
            cmake .. 2>&1 | tail -10
          fi

          if ! make -j$(nproc) manufacturing_event_generator 2>&1 | tail -10; then
            echo ""
            echo "✗ Build failed"
            echo "To see full build output, run:"
            echo "  cd examples/caf/build && cmake .. && make manufacturing_event_generator"
            sleep infinity
            exit 1
          fi
          cd ../../..
        fi

        echo "✓ Build complete"
        echo ""

        # Wait for Aeron driver to be ready
        echo "Waiting for Aeron media driver..."
        AERON_DIR="/dev/shm/aeron-$(whoami)"
        MAX_WAIT=30
        ELAPSED=0

        while [ $ELAPSED -lt $MAX_WAIT ]; do
          if [ -d "$AERON_DIR" ] && [ -f "$AERON_DIR/cnc.dat" ]; then
            echo "✓ Aeron media driver is ready"
            break
          fi
          sleep 1
          ELAPSED=$((ELAPSED + 1))
          if [ $((ELAPSED % 5)) -eq 0 ]; then
            echo "  Still waiting for Aeron... ($ELAPSED seconds)"
          fi
        done

        if [ $ELAPSED -ge $MAX_WAIT ]; then
          echo "✗ Timeout waiting for Aeron media driver"
          echo "  Make sure the aeron-driver process is running"
          sleep infinity
          exit 1
        fi

        echo ""
        echo "Starting lights-out manufacturing facility simulation..."
        echo "  Total machines:        25"
        echo "  Equipment types:       5"
        echo ""
        echo "  Equipment breakdown:"
        echo "    5 machines: cnc-mill-   (10Hz telemetry)"
        echo "    5 machines: cnc-lathe-  (10Hz telemetry)"
        echo "    5 machines: welder-     (5Hz telemetry)"
        echo "    5 machines: assembly-   (2Hz telemetry)"
        echo "    5 machines: inspect-    (1Hz telemetry)"
        echo ""
        echo "  Features:"
        echo "    - Realistic telemetry with noise"
        echo "    - Probabilistic degradation detection"
        echo "    - Automatic maintenance scheduling"
        echo "    - FSM state transitions"
        echo ""
        echo "Monitor with:"
        echo "  ps aux | grep manufacturing_event_generator"
        echo "  top -p \$(pgrep manufacturing_event_generator)"
        echo ""
        echo "══════════════════════════════════════════════════════════════"
        echo ""
        echo "Running continuously for profiling... Press Ctrl+C to stop."
        echo ""

        # Run continuously for profiling
        ./examples/caf/build/manufacturing_event_generator
        EXIT_CODE=$?

        echo ""
        if [ $EXIT_CODE -eq 0 ]; then
          echo "✓ Manufacturing Simulator exited cleanly (exit code: 0)"
        elif [ $EXIT_CODE -eq 130 ]; then
          echo "✓ Manufacturing Simulator stopped by user (Ctrl+C)"
        else
          echo "✗ Manufacturing Simulator failed with exit code $EXIT_CODE"
        fi
        echo ""

        # Keep process alive so devenv doesn't restart it
        echo "Manufacturing Simulator: Sleeping (process will restart if devenv reloads)..."
        sleep infinity
      '';
    };
  };

  packages = with pkgs; [
    # Build tools
    ansible
    autoconf
    automake
    gnumake
    gnupatch
    libtool
    pkg-config
    cmake
    ninja

    # Compilers and debugger
    gcc
    gdb

    # Basic utilities
    curl
    flex
    gh
    git
    jq                  # JSON processor (for API queries)
    lsof
    lsb-release
    perl
    (python311.withPackages (ps: with ps; [ rich ]))
    rsync
    unzip
    vim
    which
    xxd

    # Test support
    #caf
    aeron-cpp          # Aeron C++ messaging library for IPC
    wrangler

    # Kerberos (full package with KDC server tools)
    krb5Full

    # SASL (required for Kudu)
    cyrus_sasl

    # DNS Server (BIND9)
    bind

    # SSL/TLS
    openssl

    # Java
    jdk8_headless

    # NTP for time synchronization
    ntp

    # Infrastructure as Code (Cloudflare + FreeIPA management)
    opentofu           # Terraform fork (fully open source)
    tflint             # Terraform/OpenTofu linter
    terraform-docs     # Generate Terraform documentation
    terrascan          # Security scanner for IaC

    # Cloudflare tools
    cloudflared        # Cloudflare Tunnel client (optional)

    # Additional dependencies for docs (optional)
    graphviz
    ruby
    zlib
  ];

  # Set up environment for Kudu build
  env = {
    # CMake will search these paths for dependencies
    CMAKE_PREFIX_PATH = lib.makeSearchPath ":" [
      "${pkgs.cyrus_sasl.dev}"
      "${pkgs.cyrus_sasl.out}"
      "${pkgs.krb5Full.dev}"
      "${pkgs.krb5Full}"
      "${pkgs.aeron-cpp}"
    ];
    # Ensure libraries can be found
    CMAKE_LIBRARY_PATH = lib.makeSearchPath "/lib" [
      "${pkgs.glibc}"
      "${pkgs.krb5Full}"
      "${pkgs.cyrus_sasl}"
    ];
  };

  enterShell = ''
    # Add thirdparty cmake to PATH if it exists
    if [ -d thirdparty/installed/common/bin ]; then
      export PATH="$PWD/thirdparty/installed/common/bin:$PATH"
    fi

    echo "Kudu development environment loaded"
    echo "- C++ compiler: $(which g++)"
    echo "- CMake: $(which cmake)"
    echo "- Java: $(which java)"
    echo ""
    echo "Quick start:"
    echo "  devenv up                               - Start Kudu cluster with process manager"
    echo "  USE_INSTALLED=1 devenv up               - Force using installed binaries"
    echo "  devenv tasks run kudu:build-debug       - Build debug version"
    echo "  devenv tasks run kudu:install           - Install to DEV_INSTALL_DIR"
    echo ""
    echo "Build tasks:"
    echo "  devenv tasks run kudu:build-debug       - Build debug (auto-configures)"
    echo "  devenv tasks run kudu:build-release     - Build release (auto-configures)"
    echo "  devenv tasks run kudu:configure-debug   - Configure debug build only"
    echo "  devenv tasks run kudu:configure-release - Configure release build only"
    echo "  devenv tasks run kudu:test              - Run tests"
    echo "  devenv tasks run kudu:clean             - Clean build directories"
    echo ""
    echo "Cluster management (alternative to 'devenv up'):"
    echo "  devenv tasks run kudu:start-cluster     - Start local Kudu cluster"
    echo "  devenv tasks run kudu:stop-cluster      - Stop local cluster"
    echo "  devenv tasks run kudu:cluster-status    - Check cluster health"
    echo ""
    echo "Kerberos + DNS (optional services):"
    echo "  Set ENABLE_KERBEROS=true in .env        - Enable Kerberos KDC"
    echo "  Set ENABLE_BIND_DNS=true in .env        - Enable BIND DNS server"
    echo "  export KRB5_CONFIG=\$PWD/.devenv/kerberos/krb5.conf"
    echo "  dig @127.0.0.1 -p 5353 <hostname>       - Query local DNS (port 5353)"
    echo ""
    echo "CAF Event Sourcing Example:"
    echo "  devenv tasks run kudu:build-caf-example - Build CAF example"
    echo "  Set ENABLE_CAF_EXAMPLE=true in .env     - Run with 'devenv up'"
    echo "  ./examples/caf/build/kudu_caf_example   - Run standalone"
    echo ""
    echo "Demo:"
    echo "  devenv tasks run kudu:demo-cluster      - Clean up and prepare for demo"
  '';

  enterTest = ''
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║  KUDU CAF/AERON INTEGRATION TEST SUITE                      ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo ""

    # Ensure CAF example build directory exists
    if [ ! -d examples/caf/build ]; then
      echo "ERROR: CAF example build directory not found"
      echo "Run: devenv tasks run kudu:build-caf-example"
      exit 1
    fi

    cd examples/caf/build

    # Check if test executable exists
    if [ ! -x ./test_event_sourcing ]; then
      echo "Test executable not found. Building test_event_sourcing..."
      if ! cmake .. 2>&1 | tail -5; then
        echo "ERROR: CMake configuration failed"
        exit 1
      fi
      if ! make -j$(nproc) test_event_sourcing 2>&1 | tail -10; then
        echo "ERROR: Build failed"
        exit 1
      fi
    fi

    echo "Running CAF/Aeron Event Sourcing Integration Test..."
    echo "──────────────────────────────────────────────────────────────"
    echo ""

    # Run the test with timeout
    if ! timeout 30 ./test_event_sourcing; then
      EXIT_CODE=$?
      echo ""
      echo "══════════════════════════════════════════════════════════════"
      if [ $EXIT_CODE -eq 124 ]; then
        echo "ERROR: Test timed out after 30 seconds"
      else
        echo "ERROR: Test failed with exit code $EXIT_CODE"
      fi
      exit $EXIT_CODE
    fi

    echo ""
    echo "══════════════════════════════════════════════════════════════"
    echo "All tests passed successfully"
    echo "══════════════════════════════════════════════════════════════"
  '';

  tasks = {
    # Build third-party dependencies
    "kudu:build-thirdparty" = {
      exec = ''
        echo "Building third-party dependencies..."
        thirdparty/build-if-necessary.sh
      '';
      description = "Build Kudu third-party dependencies";
    };

    # Configure debug build
    "kudu:configure-debug" = {
      exec = ''
        echo "Configuring debug build..."
        mkdir -p build/debug
        cd build/debug

        # Set paths for CMake to find dependencies
        PREFIX_PATH="${pkgs.cyrus_sasl.dev};${pkgs.cyrus_sasl.out};${pkgs.krb5.dev};${pkgs.krb5}"

        # Explicitly use thirdparty LLVM (not system LLVM)
        LLVM_CMAKE_DIR="$(pwd)/../../thirdparty/installed/uninstrumented/lib/cmake/llvm"

        # Use thirdparty cmake if available
        if [ -f ../../thirdparty/installed/common/bin/cmake ]; then
          ../../thirdparty/installed/common/bin/cmake -DCMAKE_BUILD_TYPE=debug -DCMAKE_PREFIX_PATH="$PREFIX_PATH" -DLLVM_DIR="$LLVM_CMAKE_DIR" ../..
        else
          cmake -DCMAKE_BUILD_TYPE=debug -DCMAKE_PREFIX_PATH="$PREFIX_PATH" -DLLVM_DIR="$LLVM_CMAKE_DIR" ../..
        fi
      '';
      description = "Configure Kudu debug build with CMake";
    };

    # Build debug version
    "kudu:build-debug" = {
      exec = ''
        echo "Building debug version..."

        # Build thirdparty if needed
        thirdparty/build-if-necessary.sh

        # Configure if needed
        if [ ! -f build/debug/Makefile ]; then
          echo "Configuring debug build..."
          mkdir -p build/debug
          cd build/debug

          # Set paths for CMake to find dependencies
          PREFIX_PATH="${pkgs.cyrus_sasl.dev};${pkgs.cyrus_sasl.out};${pkgs.krb5.dev};${pkgs.krb5}"

          # Explicitly use thirdparty LLVM (not system LLVM)
          LLVM_CMAKE_DIR="$(pwd)/../../thirdparty/installed/uninstrumented/lib/cmake/llvm"

          # Use thirdparty cmake if available
          if [ -f ../../thirdparty/installed/common/bin/cmake ]; then
            ../../thirdparty/installed/common/bin/cmake -DCMAKE_BUILD_TYPE=debug -DCMAKE_PREFIX_PATH="$PREFIX_PATH" -DLLVM_DIR="$LLVM_CMAKE_DIR" ../..
          else
            cmake -DCMAKE_BUILD_TYPE=debug -DCMAKE_PREFIX_PATH="$PREFIX_PATH" -DLLVM_DIR="$LLVM_CMAKE_DIR" ../..
          fi
          cd ../..
        fi

        # Build with progress bar
        cd build/debug
        python3 ../../build-support/build_with_progress.py make -j$(nproc)

        echo "Debug build complete!"
      '';
      description = "Build Kudu debug version (with auto-configure)";
    };

    # Configure release build
    "kudu:configure-release" = {
      exec = ''
        echo "Configuring release build..."
        mkdir -p build/release
        cd build/release

        # Set paths for CMake to find dependencies
        PREFIX_PATH="${pkgs.cyrus_sasl.dev};${pkgs.cyrus_sasl.out};${pkgs.krb5.dev};${pkgs.krb5}"

        # Explicitly use thirdparty LLVM (not system LLVM)
        LLVM_CMAKE_DIR="$(pwd)/../../thirdparty/installed/uninstrumented/lib/cmake/llvm"

        # Use thirdparty cmake if available
        if [ -f ../../thirdparty/installed/common/bin/cmake ]; then
          ../../thirdparty/installed/common/bin/cmake -DCMAKE_BUILD_TYPE=release -DCMAKE_PREFIX_PATH="$PREFIX_PATH" -DLLVM_DIR="$LLVM_CMAKE_DIR" ../..
        else
          cmake -DCMAKE_BUILD_TYPE=release -DCMAKE_PREFIX_PATH="$PREFIX_PATH" -DLLVM_DIR="$LLVM_CMAKE_DIR" ../..
        fi
      '';
      description = "Configure Kudu release build with CMake";
    };

    # Build release version
    "kudu:build-release" = {
      exec = ''
        echo "Building release version..."

        # Build thirdparty if needed
        thirdparty/build-if-necessary.sh

        # Configure if needed
        if [ ! -f build/release/Makefile ]; then
          echo "Configuring release build..."
          mkdir -p build/release
          cd build/release

          # Set paths for CMake to find dependencies
          PREFIX_PATH="${pkgs.cyrus_sasl.dev};${pkgs.cyrus_sasl.out};${pkgs.krb5.dev};${pkgs.krb5}"

          # Explicitly use thirdparty LLVM (not system LLVM)
          LLVM_CMAKE_DIR="$(pwd)/../../thirdparty/installed/uninstrumented/lib/cmake/llvm"

          # Use thirdparty cmake if available
          if [ -f ../../thirdparty/installed/common/bin/cmake ]; then
            ../../thirdparty/installed/common/bin/cmake -DCMAKE_BUILD_TYPE=release -DCMAKE_PREFIX_PATH="$PREFIX_PATH" -DLLVM_DIR="$LLVM_CMAKE_DIR" ../..
          else
            cmake -DCMAKE_BUILD_TYPE=release -DCMAKE_PREFIX_PATH="$PREFIX_PATH" -DLLVM_DIR="$LLVM_CMAKE_DIR" ../..
          fi
          cd ../..
        fi

        # Build with progress bar
        cd build/release
        python3 ../../build-support/build_with_progress.py make -j$(nproc)

        echo "Release build complete!"
      '';
      description = "Build Kudu release version (with auto-configure)";
    };

    # Run tests
    "kudu:test" = {
      exec = ''
        if [ -f build/debug/Makefile ]; then
          echo "Running debug tests..."
          cd build/debug
          ctest -j$(nproc)
        elif [ -f build/release/Makefile ]; then
          echo "Running release tests..."
          cd build/release
          ctest -j$(nproc)
        else
          echo "Error: No build found. Configure and build first."
          exit 1
        fi
      '';
      description = "Run Kudu tests";
    };

    # Install to DEV_INSTALL_DIR
    "kudu:install" = {
      exec = ''
        # Check if DEV_INSTALL_DIR is set
        if [ -z "$DEV_INSTALL_DIR" ]; then
          echo "Error: DEV_INSTALL_DIR not set in .env file"
          exit 1
        fi

        # Expand tilde in DEV_INSTALL_DIR
        INSTALL_DIR="''${DEV_INSTALL_DIR/#\~/$HOME}"

        echo "Install destination: $INSTALL_DIR"

        # Check if build/latest exists
        if [ ! -L build/latest ]; then
          echo "No build found (build/latest symlink missing)."
          echo "Running debug build first..."

          # Just call the build-debug task logic inline
          thirdparty/build-if-necessary.sh

          mkdir -p build/debug
          cd build/debug

          PREFIX_PATH="${pkgs.cyrus_sasl.dev};${pkgs.cyrus_sasl.out};${pkgs.krb5.dev};${pkgs.krb5}"

          if [ -f ../../thirdparty/installed/common/bin/cmake ]; then
            ../../thirdparty/installed/common/bin/cmake -DCMAKE_BUILD_TYPE=debug -DCMAKE_PREFIX_PATH="$PREFIX_PATH" ../..
          else
            cmake -DCMAKE_BUILD_TYPE=debug -DCMAKE_PREFIX_PATH="$PREFIX_PATH" ../..
          fi

          make -j$(nproc)
          cd ../..
        fi

        # Determine build type from symlink target
        BUILD_TYPE=$(basename $(readlink build/latest))
        echo "Installing $BUILD_TYPE build from build/latest to $INSTALL_DIR..."

        # Create destination directory
        mkdir -p "$INSTALL_DIR"

        # Install using make install from the latest build
        cd build/latest
        make install DESTDIR="$INSTALL_DIR"

        echo ""
        echo "Installation complete!"
        echo "Binaries installed to: $INSTALL_DIR"
      '';
      description = "Install Kudu to DEV_INSTALL_DIR (from .env file)";
    };

    # Clean build directories
    "kudu:clean" = {
      exec = ''
        echo "Cleaning build directories..."
        rm -rf build/debug build/release
        echo "Build directories cleaned."
      '';
      description = "Clean Kudu build directories";
    };

    # Start local Kudu cluster
    "kudu:start-cluster" = {
      exec = ''
        # Expand tilde in CLUSTER_DIR
        CLUSTER_DIR_EXPANDED="''${CLUSTER_DIR/#\~/$HOME}"

        # Check what's available
        HAS_BUILD=false
        HAS_INSTALL=false

        if [ -L build/latest ] && [ -x build/latest/bin/kudu-master ]; then
          HAS_BUILD=true
        fi

        if [ -n "$DEV_INSTALL_DIR" ]; then
          INSTALL_DIR="''${DEV_INSTALL_DIR/#\~/$HOME}"
          # CMake installs to usr/local/sbin
          if [ -x "$INSTALL_DIR/usr/local/sbin/kudu-master" ]; then
            HAS_INSTALL=true
          fi
        fi

        # Determine which to use
        USE_INSTALL=false
        USE_BUILD=false

        if [ "$HAS_BUILD" = true ] && [ "$HAS_INSTALL" = true ]; then
          # Both available - check environment variable or default to build
          if [ "$USE_INSTALLED" = "1" ]; then
            echo "Using installation (USE_INSTALLED=1): $INSTALL_DIR"
            USE_INSTALL=true
          elif [ -t 0 ]; then
            # Interactive terminal - prompt user
            echo "Both local build and installation are available:"
            echo "  1) Use local build (build/latest)"
            echo "  2) Use installation ($INSTALL_DIR)"
            read -p "Choose [1-2] (default: 1): " choice
            case "$choice" in
              2)
                USE_INSTALL=true
                ;;
              *)
                USE_BUILD=true
                ;;
            esac
          else
            # Non-interactive - default to build
            echo "Both local build and installation available, defaulting to build (set USE_INSTALLED=1 for installation)"
            USE_BUILD=true
          fi
        elif [ "$HAS_BUILD" = true ]; then
          USE_BUILD=true
        elif [ "$HAS_INSTALL" = true ]; then
          USE_INSTALL=true
        else
          echo "Error: No Kudu build or installation found."
          echo "Run 'devenv tasks run kudu:build-debug' or 'devenv tasks run kudu:install' first."
          exit 1
        fi

        # Start the cluster
        echo ""
        echo "Cluster data directory: $CLUSTER_DIR_EXPANDED"
        echo "Masters: ''${NUM_MASTERS:-1}, Tablet Servers: ''${NUM_TSERVERS:-3}"
        echo "RPC bind address: ''${RPC_IP:-127.0.0.1}"
        echo "Web UI address: ''${WEB_IP:-127.0.0.1}"
        echo ""

        if [ "$USE_INSTALL" = true ]; then
          echo "Starting cluster from installation: $INSTALL_DIR"
          # CMake installs to usr/local, so pass that as installdir
          # Set KUDU_HOME to source tree so web UI can find www directory
          export KUDU_HOME="$PWD"
          # Set LD_LIBRARY_PATH to find shared libraries from build and thirdparty directories
          # (internal libraries like libmaster.so aren't installed, only public libkudu_client.so)
          LIB_PATHS=""
          if [ -L build/latest ]; then
            BUILD_LIB_PATH="$(cd build/latest/lib && pwd)"
            LIB_PATHS="$BUILD_LIB_PATH"
          fi
          if [ -d thirdparty/installed/uninstrumented/lib ]; then
            THIRDPARTY_LIB_PATH="$(cd thirdparty/installed/uninstrumented/lib && pwd)"
            LIB_PATHS="''${LIB_PATHS:+$LIB_PATHS:}$THIRDPARTY_LIB_PATH"
          fi
          if [ -n "$LIB_PATHS" ]; then
            export LD_LIBRARY_PATH="$LIB_PATHS:''${LD_LIBRARY_PATH:-}"
          fi
          START_ARGS="--installdir $INSTALL_DIR/usr/local \
            --clusterdir $CLUSTER_DIR_EXPANDED \
            --num-masters ''${NUM_MASTERS:-1} \
            --num-tservers ''${NUM_TSERVERS:-3} \
            --host ''${RPC_IP:-127.0.0.1} \
            --webhost ''${WEB_IP:-127.0.0.1}"
          # Note: --webadvertised flag is not supported by start_kudu.sh
          if [ -n "$EXTRA_MASTER_FLAGS" ]; then
            START_ARGS="$START_ARGS --master-flags \"$EXTRA_MASTER_FLAGS\""
          fi
          if [ -n "$EXTRA_TSERVER_FLAGS" ]; then
            START_ARGS="$START_ARGS --tserver-flags \"$EXTRA_TSERVER_FLAGS\""
          fi
          src/kudu/scripts/start_kudu.sh $START_ARGS
        else
          BUILD_PATH="$(cd build/latest && pwd)"
          echo "Starting cluster from build: $BUILD_PATH"
          START_ARGS="--builddir $BUILD_PATH \
            --clusterdir $CLUSTER_DIR_EXPANDED \
            --num-masters ''${NUM_MASTERS:-1} \
            --num-tservers ''${NUM_TSERVERS:-3} \
            --host ''${RPC_IP:-127.0.0.1} \
            --webhost ''${WEB_IP:-127.0.0.1}"
          # Note: --webadvertised flag is not supported by start_kudu.sh
          if [ -n "$EXTRA_MASTER_FLAGS" ]; then
            START_ARGS="$START_ARGS --master-flags \"$EXTRA_MASTER_FLAGS\""
          fi
          if [ -n "$EXTRA_TSERVER_FLAGS" ]; then
            START_ARGS="$START_ARGS --tserver-flags \"$EXTRA_TSERVER_FLAGS\""
          fi
          src/kudu/scripts/start_kudu.sh $START_ARGS
        fi

        echo ""
        echo "Cluster started!"
        echo "Master web UI: http://''${WEB_IP:-127.0.0.1}:8765/"
        echo "Master addresses: ''${RPC_IP:-127.0.0.1}:8764"
      '';
      description = "Start local Kudu cluster";
    };

    # Stop local Kudu cluster
    "kudu:stop-cluster" = {
      exec = ''
        echo "Stopping Kudu cluster..."
        src/kudu/scripts/stop_kudu.sh
        echo "Cluster stopped."
      '';
      description = "Stop local Kudu cluster";
    };

    # Check cluster status
    "kudu:cluster-status" = {
      exec = ''
        # Try to find kudu binary
        KUDU_BIN=""
        if [ -L build/latest ] && [ -x build/latest/bin/kudu ]; then
          KUDU_BIN="build/latest/bin/kudu"
        elif [ -n "$DEV_INSTALL_DIR" ]; then
          INSTALL_DIR="''${DEV_INSTALL_DIR/#\~/$HOME}"
          # CMake installs client tool to usr/local/bin
          if [ -x "$INSTALL_DIR/usr/local/bin/kudu" ]; then
            KUDU_BIN="$INSTALL_DIR/usr/local/bin/kudu"
          fi
        fi

        if [ -z "$KUDU_BIN" ]; then
          echo "Error: Cannot find kudu binary. Build or install Kudu first."
          exit 1
        fi

        echo "Checking cluster status..."
        echo ""

        # Set KUDU_USER_NAME for non-secure environment
        export KUDU_USER_NAME=kudu

        # Check cluster health
        "$KUDU_BIN" cluster ksck ''${RPC_IP:-127.0.0.1}:8764

        echo ""
        echo "To view the master web UI, visit: http://''${WEB_IP:-127.0.0.1}:8765/"
      '';
      description = "Check local Kudu cluster health";
    };

    # Build CAF event sourcing example
    "kudu:build-caf-example" = {
      exec = ''
        echo "Building CAF event sourcing example..."

        # Ensure thirdparty is built
        if [ ! -d thirdparty/installed/uninstrumented ]; then
          echo "Building thirdparty dependencies first..."
          thirdparty/build-if-necessary.sh
        fi

        # Ensure Kudu build exists
        if [ ! -L build/latest ]; then
          echo "No Kudu build found. Building debug version..."
          # Run build-debug task logic
          thirdparty/build-if-necessary.sh
          mkdir -p build/debug
          cd build/debug
          PREFIX_PATH="${pkgs.cyrus_sasl.dev};${pkgs.cyrus_sasl.out};${pkgs.krb5.dev};${pkgs.krb5}"
          if [ -f ../../thirdparty/installed/common/bin/cmake ]; then
            ../../thirdparty/installed/common/bin/cmake -DCMAKE_BUILD_TYPE=debug -DCMAKE_PREFIX_PATH="$PREFIX_PATH" ../..
          else
            cmake -DCMAKE_BUILD_TYPE=debug -DCMAKE_PREFIX_PATH="$PREFIX_PATH" ../..
          fi
          make -j$(nproc)
          cd ../..
        fi

        # Build CAF example
        echo "Configuring CAF example..."
        mkdir -p examples/caf/build
        cd examples/caf/build

        # CAF and other dependencies need to be found via CMAKE_PREFIX_PATH
        # Use thirdparty cmake if available, disable tests by default
        if [ -f ../../../thirdparty/installed/common/bin/cmake ]; then
          ../../../thirdparty/installed/common/bin/cmake .. \
            -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH" \
            -DBUILD_TESTS=OFF
        else
          cmake .. \
            -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH" \
            -DBUILD_TESTS=OFF
        fi

        echo "Building CAF example..."
        make -j$(nproc) kudu_caf_example

        if [ -x kudu_caf_example ]; then
          echo ""
          echo "CAF example built successfully!"
          echo "Executable: examples/caf/build/kudu_caf_example"
          echo ""
          echo "Run with: ./examples/caf/build/kudu_caf_example --help"
        else
          echo "Error: Build failed, executable not found"
          exit 1
        fi
      '';
      description = "Build Kudu CAF event sourcing example";
    };

    # Demo cleanup - prepare for Cloudera demo
    "kudu:demo-cluster" = {
      exec = ''
        echo "╔══════════════════════════════════════════════════════════════╗"
        echo "║  KUDU DEMO CLUSTER PREPARATION                               ║"
        echo "║  Cleaning up for Cloudera demonstration                      ║"
        echo "╚══════════════════════════════════════════════════════════════╝"
        echo ""

        # 1. Stop any running devenv processes
        echo "Step 1: Stopping any running devenv processes..."
        if pgrep -f "devenv up" > /dev/null 2>&1; then
          echo "  Found running devenv up processes, stopping..."
          pkill -f "devenv up" 2>/dev/null || true
          sleep 2
        else
          echo "  No devenv up processes running"
        fi
        echo ""

        # 2. Kill any orphaned Kudu processes
        echo "Step 2: Cleaning up Kudu processes..."
        KUDU_PROCS=$(pgrep -f "kudu-master|kudu-tserver|kudu_service" 2>/dev/null || true)
        if [ -n "$KUDU_PROCS" ]; then
          echo "  Found Kudu processes: $KUDU_PROCS"
          pkill -f "kudu-master" 2>/dev/null || true
          pkill -f "kudu-tserver" 2>/dev/null || true
          pkill -f "kudu_service" 2>/dev/null || true
          sleep 2
          # Force kill if still running
          pkill -9 -f "kudu-master" 2>/dev/null || true
          pkill -9 -f "kudu-tserver" 2>/dev/null || true
          pkill -9 -f "kudu_service" 2>/dev/null || true
          echo "  ✓ Kudu processes terminated"
        else
          echo "  No Kudu processes running"
        fi
        echo ""

        # 3. Kill CAF example processes
        echo "Step 3: Cleaning up CAF example processes..."
        CAF_PROCS=$(pgrep -f "test_event_sourcing|kudu_caf_example|manufacturing_event" 2>/dev/null || true)
        if [ -n "$CAF_PROCS" ]; then
          echo "  Found CAF processes: $CAF_PROCS"
          pkill -f "test_event_sourcing" 2>/dev/null || true
          pkill -f "kudu_caf_example" 2>/dev/null || true
          pkill -f "manufacturing_event" 2>/dev/null || true
          sleep 1
          echo "  ✓ CAF processes terminated"
        else
          echo "  No CAF processes running"
        fi
        echo ""

        # 4. Kill Aeron media driver
        echo "Step 4: Cleaning up Aeron media driver..."
        AERON_PROCS=$(pgrep -f "aeronmd" 2>/dev/null || true)
        if [ -n "$AERON_PROCS" ]; then
          echo "  Found Aeron driver processes: $AERON_PROCS"
          pkill -f "aeronmd" 2>/dev/null || true
          sleep 1
          echo "  ✓ Aeron driver terminated"
        else
          echo "  No Aeron driver running"
        fi
        echo ""

        # 5. Clean up Aeron shared memory
        echo "Step 5: Cleaning up Aeron shared memory..."
        AERON_DIR="/dev/shm/aeron-$(whoami)"
        if [ -d "$AERON_DIR" ]; then
          echo "  Removing $AERON_DIR"
          rm -rf "$AERON_DIR"
          echo "  ✓ Aeron shared memory cleaned"
        else
          echo "  No Aeron shared memory to clean"
        fi
        echo ""

        # 6. Clean up Kudu cluster data (optional - controlled by env var)
        echo "Step 6: Cluster data management..."
        CLUSTER_DIR_EXPANDED="''${CLUSTER_DIR/#\~/$HOME}"
        if [ "''${DEMO_CLEAN_DATA:-false}" = "true" ]; then
          if [ -d "$CLUSTER_DIR_EXPANDED" ]; then
            echo "  DEMO_CLEAN_DATA=true: Removing cluster data at $CLUSTER_DIR_EXPANDED"
            rm -rf "$CLUSTER_DIR_EXPANDED"
            echo "  ✓ Cluster data removed (will start fresh)"
          fi
        else
          echo "  Keeping existing cluster data at $CLUSTER_DIR_EXPANDED"
          echo "  (Set DEMO_CLEAN_DATA=true to remove)"
        fi
        echo ""

        # 7. Verify no processes remain
        echo "Step 7: Verifying cleanup..."
        REMAINING=$(pgrep -f "kudu-master|kudu-tserver|aeronmd|test_event_sourcing" 2>/dev/null || true)
        if [ -n "$REMAINING" ]; then
          echo "  ⚠ Warning: Some processes still running: $REMAINING"
          echo "  You may need to manually kill these"
        else
          echo "  ✓ All processes cleaned up"
        fi
        echo ""

        # 8. Check build readiness
        echo "Step 8: Checking build readiness..."
        if [ -L build/latest ] && [ -x build/latest/bin/kudu-master ]; then
          BUILD_TYPE=$(basename $(readlink build/latest))
          echo "  ✓ Kudu $BUILD_TYPE build ready at build/latest"
        else
          echo "  ⚠ No Kudu build found. Run: devenv tasks run kudu:build-release"
        fi

        if [ -x examples/caf/build/test_event_sourcing ]; then
          echo "  ✓ CAF chaos testing example ready"
        else
          echo "  ⚠ CAF example not built. Run: devenv tasks run kudu:build-caf-example"
        fi
        echo ""

        echo "══════════════════════════════════════════════════════════════"
        echo ""
        echo "Demo environment ready!"
        echo ""
        echo "To start the demo, run:"
        echo "  devenv up"
        echo ""
        echo "This will start:"
        echo "  • Aeron media driver (IPC transport)"
        echo "  • Kudu cluster (master + tablet servers)"
        echo "  • Kudu service (Aeron-to-Kudu bridge)"
        echo "  • CAF chaos testing (continuous with RTO/RPO metrics)"
        echo "  • Manufacturing simulator (optional)"
        echo ""
        echo "Demo features:"
        echo "  • Continuous chaos injection (actor kills, delays)"
        echo "  • Real-time RTO/RPO measurement"
        echo "  • Hourly/daily assessment reports"
        echo "  • Full event sourcing through Kudu"
        echo ""
        echo "Configuration (via .env or environment):"
        echo "  CHAOS_MODE=CONTINUOUS        Run indefinitely"
        echo "  NUM_ACTORS=100               Number of equipment actors"
        echo "  CHAOS_FAILURE_RATE_PER_MIN=10  Failures per minute"
        echo "  METRICS_CONSOLE_INTERVAL_SEC=10  Metrics output interval"
        echo ""
        echo "══════════════════════════════════════════════════════════════"
      '';
      description = "Clean up and prepare for Cloudera demo";
    };
  };
}

