{ pkgs, lib, config, inputs, ... }:

{
  dotenv.enable = true;

  # Long-running processes managed by 'devenv up'
  processes = {
    kudu-cluster.exec = ''
      # Determine which directory to use: build or install
      DIR_ARG=""
      if [ "$USE_INSTALLED" = "1" ] || [ "$USE_INSTALLED" = "true" ]; then
        # User explicitly requested installed version
        if [ -n "$DESTDIR" ]; then
          INSTALL_DIR="''${DESTDIR/#\~/$HOME}"
          if [ -x "$INSTALL_DIR/usr/local/sbin/kudu-master" ]; then
            DIR_ARG="$INSTALL_DIR/usr/local"
          else
            echo "Error: USE_INSTALLED=1 but no installation found at $INSTALL_DIR"
            echo "Run 'devenv tasks run kudu:install' first."
            exit 1
          fi
        else
          echo "Error: USE_INSTALLED=1 but DESTDIR not set in .env"
          exit 1
        fi
      elif [ -L build/latest ] && [ -x build/latest/bin/kudu-master ]; then
        # Use build directory (preferred)
        DIR_ARG="$(cd build/latest && pwd)"
      elif [ -n "$DESTDIR" ]; then
        # Fall back to installation
        INSTALL_DIR="''${DESTDIR/#\~/$HOME}"
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
  };

  packages = with pkgs; [
    # Build tools
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
    git
    lsof
    lsb-release
    perl
    (python311.withPackages (ps: with ps; [ rich ]))
    rsync
    unzip
    vim
    which
    xxd

    # Kerberos
    krb5

    # SASL (required for Kudu)
    cyrus_sasl

    # SSL/TLS
    openssl_3

    # Java
    jdk8_headless

    # NTP for time synchronization
    ntp

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
      "${pkgs.krb5.dev}"
      "${pkgs.krb5}"
    ];
    # Ensure libraries can be found
    CMAKE_LIBRARY_PATH = lib.makeSearchPath "/lib" [
      "${pkgs.glibc}"
      "${pkgs.krb5}"
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
    echo "  devenv tasks run kudu:install           - Install to DESTDIR"
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

        # Use thirdparty cmake if available
        if [ -f ../../thirdparty/installed/common/bin/cmake ]; then
          ../../thirdparty/installed/common/bin/cmake -DCMAKE_BUILD_TYPE=debug -DCMAKE_PREFIX_PATH="$PREFIX_PATH" ../..
        else
          cmake -DCMAKE_BUILD_TYPE=debug -DCMAKE_PREFIX_PATH="$PREFIX_PATH" ../..
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

          # Use thirdparty cmake if available
          if [ -f ../../thirdparty/installed/common/bin/cmake ]; then
            ../../thirdparty/installed/common/bin/cmake -DCMAKE_BUILD_TYPE=debug -DCMAKE_PREFIX_PATH="$PREFIX_PATH" ../..
          else
            cmake -DCMAKE_BUILD_TYPE=debug -DCMAKE_PREFIX_PATH="$PREFIX_PATH" ../..
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

        # Use thirdparty cmake if available
        if [ -f ../../thirdparty/installed/common/bin/cmake ]; then
          ../../thirdparty/installed/common/bin/cmake -DCMAKE_BUILD_TYPE=release -DCMAKE_PREFIX_PATH="$PREFIX_PATH" ../..
        else
          cmake -DCMAKE_BUILD_TYPE=release -DCMAKE_PREFIX_PATH="$PREFIX_PATH" ../..
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

          # Use thirdparty cmake if available
          if [ -f ../../thirdparty/installed/common/bin/cmake ]; then
            ../../thirdparty/installed/common/bin/cmake -DCMAKE_BUILD_TYPE=release -DCMAKE_PREFIX_PATH="$PREFIX_PATH" ../..
          else
            cmake -DCMAKE_BUILD_TYPE=release -DCMAKE_PREFIX_PATH="$PREFIX_PATH" ../..
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

    # Install to DESTDIR
    "kudu:install" = {
      exec = ''
        # Check if DESTDIR is set
        if [ -z "$DESTDIR" ]; then
          echo "Error: DESTDIR not set in .env file"
          exit 1
        fi

        # Expand tilde in DESTDIR
        INSTALL_DIR="''${DESTDIR/#\~/$HOME}"

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
      description = "Install Kudu to DESTDIR (from .env file)";
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

        if [ -n "$DESTDIR" ]; then
          INSTALL_DIR="''${DESTDIR/#\~/$HOME}"
          # CMake installs to usr/local/sbin
          if [ -x "$INSTALL_DIR/usr/local/sbin/kudu-master" ]; then
            HAS_INSTALL=true
          fi
        fi

        # Determine which to use
        USE_INSTALL=false
        USE_BUILD=false

        if [ "$HAS_BUILD" = true ] && [ "$HAS_INSTALL" = true ]; then
          # Both available - prompt user
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
          if [ -n "$WEB_ADVERTISED_IP" ]; then
            START_ARGS="$START_ARGS --webadvertised $WEB_ADVERTISED_IP"
          fi
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
          if [ -n "$WEB_ADVERTISED_IP" ]; then
            START_ARGS="$START_ARGS --webadvertised $WEB_ADVERTISED_IP"
          fi
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
        elif [ -n "$DESTDIR" ]; then
          INSTALL_DIR="''${DESTDIR/#\~/$HOME}"
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
  };
}

