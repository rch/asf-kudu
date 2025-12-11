#!/usr/bin/env bash
#
# Simplified Kudu cluster startup script for devenv/Nix environments.
# This wrapper handles environment-specific issues like OpenSSL 3.x incompatibility
# and avoids complex argument passing through multiple shell layers.

set -e

# Default values
NUM_MASTERS=${NUM_MASTERS:-1}
NUM_TSERVERS=${NUM_TSERVERS:-3}
WEB_IP=${WEB_IP:-0.0.0.0}
RPC_IP=${RPC_IP:-127.0.0.1}
MASTER_RPC_PORT_BASE=8764
TSERVER_RPC_PORT_BASE=9870

# Detect repository root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$REPO_ROOT"

# Determine binary locations (build vs install)
USE_BUILD=false
USE_INSTALL=false
BUILD_DIR=""
INSTALL_DIR=""

if [ -n "$1" ]; then
  # Argument provided: use as build or install directory
  if [ -x "$1/bin/kudu-master" ]; then
    # Build directory
    BUILD_DIR="$1"
    USE_BUILD=true
  elif [ -x "$1/sbin/kudu-master" ]; then
    # Install directory
    INSTALL_DIR="$1"
    USE_INSTALL=true
  else
    echo "Error: Cannot find kudu-master in $1/bin or $1/sbin"
    exit 1
  fi
else
  # Auto-detect: prefer build/latest
  if [ -L build/latest ] && [ -x build/latest/bin/kudu-master ]; then
    BUILD_DIR="$(cd build/latest && pwd)"
    USE_BUILD=true
  elif [ -n "$DESTDIR" ]; then
    DESTDIR_EXPANDED="${DESTDIR/#\~/$HOME}"
    if [ -x "$DESTDIR_EXPANDED/usr/local/sbin/kudu-master" ]; then
      INSTALL_DIR="$DESTDIR_EXPANDED/usr/local"
      USE_INSTALL=true
    fi
  fi
fi

if [ "$USE_BUILD" = false ] && [ "$USE_INSTALL" = false ]; then
  echo "Error: No Kudu build or installation found."
  echo "Run 'devenv tasks run kudu:build-debug' first."
  exit 1
fi

# Set up binaries
if [ "$USE_BUILD" = true ]; then
  KUDU_MASTER="$BUILD_DIR/bin/kudu-master"
  KUDU_TSERVER="$BUILD_DIR/bin/kudu-tserver"
  WEBSERVER_DOC_ROOT="$REPO_ROOT/www"
  echo "Using build from: $BUILD_DIR"

  # Set up library paths for build directory
  LIB_PATHS="$BUILD_DIR/lib"
  if [ -d "$REPO_ROOT/thirdparty/installed/uninstrumented/lib" ]; then
    LIB_PATHS="$LIB_PATHS:$REPO_ROOT/thirdparty/installed/uninstrumented/lib"
  fi
  export LD_LIBRARY_PATH="$LIB_PATHS:${LD_LIBRARY_PATH:-}"
else
  KUDU_MASTER="$INSTALL_DIR/sbin/kudu-master"
  KUDU_TSERVER="$INSTALL_DIR/sbin/kudu-tserver"
  WEBSERVER_DOC_ROOT="$REPO_ROOT/www"
  echo "Using installation from: $INSTALL_DIR"

  # Set up library paths for installed binaries
  # (internal libs not installed, need build + thirdparty)
  LIB_PATHS=""
  if [ -L build/latest ]; then
    LIB_PATHS="$(cd build/latest/lib && pwd)"
  fi
  if [ -d "$REPO_ROOT/thirdparty/installed/uninstrumented/lib" ]; then
    LIB_PATHS="${LIB_PATHS:+$LIB_PATHS:}$(cd thirdparty/installed/uninstrumented/lib && pwd)"
  fi
  if [ -n "$LIB_PATHS" ]; then
    export LD_LIBRARY_PATH="$LIB_PATHS:${LD_LIBRARY_PATH:-}"
  fi
fi

# Set KUDU_HOME for www directory
export KUDU_HOME="$REPO_ROOT"

# Determine cluster directory
if [ -z "$CLUSTER_DIR" ]; then
  # Default to version-based tmp directory
  VERSION=""
  if [ -f version.txt ]; then
    VERSION=$(cat version.txt | tr -d '[:space:]')
  fi
  if [ -n "$VERSION" ]; then
    CLUSTER_DIR="/tmp/kudu-$VERSION"
  else
    CLUSTER_DIR="/tmp/kudu-cluster"
  fi
  echo "Using default cluster directory: $CLUSTER_DIR"
else
  CLUSTER_DIR="${CLUSTER_DIR/#\~/$HOME}"
  echo "Using cluster directory: $CLUSTER_DIR"
fi

# Calculate memory limit (80% of total RAM divided by number of processes)
NUM_PROCESSES=$((NUM_MASTERS + NUM_TSERVERS))
MEM_SIZE_KB=$(grep -E '^MemTotal' /proc/meminfo | awk '{print $2}')
MEM_SIZE_BYTES=$((MEM_SIZE_KB * 1024))
MEM_LIMIT_BYTES=$((MEM_SIZE_BYTES * 4 / 5 / NUM_PROCESSES))

# Compute master addresses
MASTER_ADDRESSES=""
for i in $(seq 0 $((NUM_MASTERS - 1))); do
  MASTER_RPC_PORT=$((MASTER_RPC_PORT_BASE + i * 2))
  if [ $i -ne 0 ]; then
    MASTER_ADDRESSES="${MASTER_ADDRESSES},"
  fi
  MASTER_ADDRESSES="${MASTER_ADDRESSES}${RPC_IP}:${MASTER_RPC_PORT}"
done

# Track PIDs for cleanup
pids=()

# Function to start a kudu-master
start_master() {
  local name=$1
  local rpc_port=$2
  local http_port=$3

  # Create directories
  local root_dir="$CLUSTER_DIR/$name"
  mkdir -p "$root_dir/data" "$root_dir/wal" "$root_dir/log"

  echo "Starting $name:"
  echo "  RPC  port $rpc_port"
  echo "  HTTP port $http_port"

  # Build command line
  local args=(
    "$KUDU_MASTER"
    --master_addresses="$MASTER_ADDRESSES"
    --fs_data_dirs="$root_dir/data"
    --fs_wal_dir="$root_dir/wal"
    --log_dir="$root_dir/log"
    --rpc_bind_addresses="$RPC_IP:$rpc_port"
    --time_source=system_unsync
    --unlock_unsafe_flags
    --webserver_interface="$WEB_IP"
    --webserver_port="$http_port"
    --webserver_doc_root="$WEBSERVER_DOC_ROOT"
    --rpc_encryption=disabled
    --rpc_authentication=disabled
  )

  # Add advertised address if set
  if [ -n "$WEB_ADVERTISED_IP" ]; then
    args+=(--webserver_advertised_addresses="$WEB_ADVERTISED_IP:$http_port")
  fi

  # Launch in background
  "${args[@]}" &
  local pid=$!
  pids+=($pid)
  echo "  PID $pid"
}

# Function to start a kudu-tserver
start_tserver() {
  local name=$1
  local rpc_port=$2
  local http_port=$3

  # Create directories
  local root_dir="$CLUSTER_DIR/$name"
  mkdir -p "$root_dir/data" "$root_dir/wal" "$root_dir/log"

  echo "Starting $name:"
  echo "  RPC  port $rpc_port"
  echo "  HTTP port $http_port"

  # Build command line
  local args=(
    "$KUDU_TSERVER"
    --fs_data_dirs="$root_dir/data"
    --fs_wal_dir="$root_dir/wal"
    --log_dir="$root_dir/log"
    --rpc_bind_addresses="$RPC_IP:$rpc_port"
    --time_source=system_unsync
    --unlock_unsafe_flags
    --webserver_interface="$WEB_IP"
    --webserver_port="$http_port"
    --tserver_master_addrs="$MASTER_ADDRESSES"
    --webserver_doc_root="$WEBSERVER_DOC_ROOT"
    --memory_limit_hard_bytes="$MEM_LIMIT_BYTES"
    --rpc_encryption=disabled
    --rpc_authentication=disabled
  )

  # Add advertised address if set
  if [ -n "$WEB_ADVERTISED_IP" ]; then
    args+=(--webserver_advertised_addresses="$WEB_ADVERTISED_IP:$http_port")
  fi

  # Launch in background
  "${args[@]}" &
  local pid=$!
  pids+=($pid)
  echo "  PID $pid"
}

# Start masters
echo ""
echo "Starting $NUM_MASTERS master(s) and $NUM_TSERVERS tablet server(s)"
echo "Cluster directory: $CLUSTER_DIR"
echo "RPC bind address: $RPC_IP"
echo "Web UI bind address: $WEB_IP"
if [ -n "$WEB_ADVERTISED_IP" ]; then
  echo "Web UI advertised address: $WEB_ADVERTISED_IP"
fi
echo ""

for i in $(seq 0 $((NUM_MASTERS - 1))); do
  MASTER_RPC_PORT=$((MASTER_RPC_PORT_BASE + i * 2))
  MASTER_HTTP_PORT=$((MASTER_RPC_PORT + 1))
  start_master "master-$i" $MASTER_RPC_PORT $MASTER_HTTP_PORT
done

# Start tservers
for i in $(seq 0 $((NUM_TSERVERS - 1))); do
  TSERVER_RPC_PORT=$((TSERVER_RPC_PORT_BASE + i * 2))
  TSERVER_HTTP_PORT=$((TSERVER_RPC_PORT + 1))
  start_tserver "tserver-$i" $TSERVER_RPC_PORT $TSERVER_HTTP_PORT
done

echo ""
echo "Cluster started!"
echo "Master web UI: http://${WEB_ADVERTISED_IP:-$WEB_IP}:8765/"
echo "Master RPC address: $RPC_IP:8764"
echo ""
echo "Press Ctrl+C to stop the cluster."
echo ""

# Cleanup handler
cleanup() {
  echo ""
  echo "Shutting down cluster..."
  for pid in "${pids[@]}"; do
    kill $pid 2>/dev/null || true
  done
  echo "Cluster stopped."
  exit 0
}

trap cleanup SIGINT SIGTERM

# Wait for all background processes
wait
