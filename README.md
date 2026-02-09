# KVStoreV2 - High-Performance Key-Value Store with gRPC

A distributed key-value store system for caching GPT prompt tokens, featuring a Windows gRPC service backed by Azure Blob Storage and a Linux client library.

## Architecture

```
┌─────────────────────┐         gRPC          ┌─────────────────────────────────────┐
│   Linux Client      │ ◄──────────────────► │  Windows Service (Multi-NUMA)       │
│                     │                       │                                     │
│  - KVPlayground     │        :8085 ────────►│  KVStoreServer (NUMA Node 0)       │
│  - KVClient lib     │        :8086 ────────►│  KVStoreServer (NUMA Node 1)       │
│                     │                       │         │                           │
└─────────────────────┘                       │         ▼                           │
                                              │  Azure Blob Storage                 │
                                              └─────────────────────────────────────┘
```

## Performance Optimizations

### NUMA-Aware Multi-Process Architecture

For VMs with multiple NUMA nodes (e.g., Azure FX96 with 96 cores / 2 NUMA nodes), run **two server instances** pinned to each NUMA node:

```powershell
# On Windows VM - start two server processes
start "KVStore-Node0" /NODE 0 .\KVStoreServer.exe --port 8085 --log-level error --disable-metrics
start "KVStore-Node1" /NODE 1 .\KVStoreServer.exe --port 8086 --log-level error --disable-metrics
```

This ensures:
- **Full CPU utilization** across all cores
- **Reduced latency variance** (no cross-NUMA memory access)
- **2x throughput** compared to single-process

### gRPC Optimizations

The server includes these performance settings:
- **TCP_NODELAY**: Disabled Nagle's algorithm for lower latency
- **Keepalive**: 10s ping interval to maintain connections
- **HTTP/2 flow control**: 64MB stream window, 16MB max frame size
- **Concurrent streams**: 200 per connection

### Azure Storage Optimizations

- **Connection pooling**: 100 concurrent HTTP connections
- **DNS caching**: 300 second TTL
- **SSL session reuse**: Enabled for faster TLS handshakes

## Projects

### 1. **KVService** (Windows)
- **Purpose**: gRPC service that manages Azure Blob Storage operations
- **Platform**: Windows x64
- **Features**:
  - Multi-NUMA support with per-node process pinning
  - Multi-NIC support with custom Azure SDK
  - CurlTransport for network optimization
  - Bloom filter for fast lookups
  - 128-token block caching
  - Static runtime (/MT)
- **Build**: `cd KVService && .\build_with_local_sdk.ps1`

### 2. **KVClient** (Linux)
- **Purpose**: gRPC client library with `AzureStorageKVStoreLibV2` interface
- **Platform**: Linux x64
- **Features**:
  - Drop-in replacement for `AzureStorageKVStoreLibV2`
  - Transparent gRPC communication
  - Same API as original library
  - Thread-safe async operations
- **Build**: See Linux build instructions below

### 3. **KVPlayground** (Linux)
- **Purpose**: Test application demonstrating KVClient usage
- **Platform**: Linux x64
- **Features**:
  - Multi-threaded testing
  - Precomputed token support
  - Performance benchmarking
  - Cache validation
- **Build**: Built automatically with KVClient

## Quick Start

### Windows (KVService)

```powershell
cd KVService
.\build_with_local_sdk.ps1

# Single server (simple setup)
.\build\Release\KVStoreServer.exe --port 8085 --log-level error --disable-metrics

# Multi-NUMA setup (for 96+ core VMs) - run BOTH commands
start "KVStore-Node0" /NODE 0 .\build\Release\KVStoreServer.exe --port 8085 --log-level error --disable-metrics
start "KVStore-Node1" /NODE 1 .\build\Release\KVStoreServer.exe --port 8086 --log-level error --disable-metrics
```

### Server Command Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `--port PORT` | gRPC listen port | 50051 |
| `--host HOST` | Bind address | 0.0.0.0 |
| `--threads NUM` | Server thread count | auto-detect |
| `--log-level LEVEL` | error, info, verbose | info |
| `--transport TYPE` | winhttp, libcurl | libcurl |
| `--disable-metrics` | Disable console metrics | enabled |
| `--enable-sdk-logging` | Enable Azure SDK logs | disabled |
| `--disable-multi-nic` | Disable NIC round-robin | enabled |

### Linux (KVClient + KVPlayground)

```bash
# Prerequisites
sudo apt-get install build-essential cmake
# Install vcpkg and gRPC (see detailed instructions below)

# Build
mkdir build && cd build
cmake ..
make

# Set server address (single server)
export KVSTORE_GRPC_SERVER="your-windows-server:8085"

# Run playground
./KVPlayground/KVPlayground conversation_tokens.json 10 5

# For multi-NUMA setup, run two clients to different ports:
# Terminal 1: export KVSTORE_GRPC_SERVER="server:8085" && ./KVPlayground ...
# Terminal 2: export KVSTORE_GRPC_SERVER="server:8086" && ./KVPlayground ...
```

## Building on Linux

### Prerequisites

1. **Install dependencies**:
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config \
    libssl-dev autoconf libtool curl zip unzip python3-tiktoken
```

2. **Install vcpkg**:
```bash
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
./vcpkg integrate install
```

3. **Install gRPC, Protobuf, Azure SDK and other important packages**:
```bash
./vcpkg install grpc protobuf nlohmann-json zlib azure-storage-blobs-cpp azure-identity-cpp
```

### Build Steps

```bash
cd kvStoreV2
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
make -j$(nproc)
```

### Running

```bash
# Set server address
export KVSTORE_GRPC_SERVER="192.168.1.100:50051"

# Run KVPlayground
cd build/KVPlayground
./KVPlayground ../../conversation_tokens.json 5 2
```

## Configuration

### Environment Variables

- **KVSTORE_GRPC_SERVER**: gRPC server address (default: `localhost:50051`)
  ```bash
  export KVSTORE_GRPC_SERVER="myserver.example.com:50051"
  ```

### KVService Configuration

Edit `KVService/src/server.cpp` or use command-line arguments:
- `--port`: gRPC server port (default: 50051)
- `--log-level`: Logging level (error, info, verbose)

## API Usage

The KVClient provides the exact same API as `AzureStorageKVStoreLibV2`:

```cpp
#include "AzureStorageKVStoreLibV2.h"

// Initialize
AzureStorageKVStoreLibV2 kvStore;
kvStore.Initialize(
    "https://account.blob.core.windows.net",
    "container-name",
    HttpTransportProtocol::LibCurl  // Ignored in gRPC client
);

// Write
PromptChunk chunk;
chunk.hash = 12345;
chunk.partitionKey = "my-partition";
chunk.tokens = {1, 2, 3, ...};  // 128 tokens
auto writeFuture = kvStore.WriteAsync(chunk);
writeFuture.wait();

// Lookup
std::vector<hash_t> hashes = {12345};
auto result = kvStore.Lookup(
    "my-partition",
    "completion-001",
    tokens.begin(),
    tokens.end(),
    hashes
);

// Read
if (result.cachedBlocks > 0) {
    auto readFuture = kvStore.ReadAsync(result.locations[0].location);
    auto [success, chunk] = readFuture.get();
}
```

## Protocol

The gRPC protocol is defined in `protos/kvstore.proto`:

- **Lookup**: Find cached token blocks
- **Read**: Retrieve cached chunks by location
- **Write**: Store new chunks

## Performance

- **Block Size**: 128 tokens
- **Caching**: Bloom filter for O(1) lookups
- **Concurrency**: Async operations, thread-safe
- **Network**: Multi-NIC support (Windows service)

## Development

### Project Structure

```
KVStoreV2/
├── CMakeLists.txt              # Root build file (Linux)
├── README.md                   # This file
├── docs/                       # Documentation
│   ├── ARCHITECTURE.md         # System architecture
│   ├── QUICKSTART.md           # Quick start guide
│   ├── SETUP-SUMMARY.md        # Setup summary
│   └── ...                     # Other docs
├── scripts/                    # All scripts organized by function
│   ├── init/                   # Initialization and build scripts
│   │   ├── init_repo.ps1
│   │   ├── init_repo.sh
│   │   └── build_linux.sh
│   ├── deploy/                 # Deployment scripts
│   │   ├── deploy_all.ps1
│   │   ├── deploy_server.ps1
│   │   ├── deploy_client.ps1
│   │   └── deploy-linux-client.ps1
│   ├── run/                    # Run scripts
│   │   ├── run_azure_linux.ps1
│   │   ├── run_azure_linux_node0.ps1
│   │   ├── run_azure_linux_node1.ps1
│   │   ├── run_azure_linux_both.ps1
│   │   ├── runLocal.ps1
│   │   └── run_local_wsl.sh
│   └── README.md
├── KVService/                  # Windows gRPC service
│   ├── src/
│   │   ├── server.cpp
│   │   ├── KVStoreServiceImpl.cpp
│   │   ├── InMemoryAccountResolver.cpp
│   │   └── reactors/
│   ├── include/
│   ├── protos/
│   ├── build_with_local_sdk.ps1
│   ├── CMakeLists.txt
│   └── README.md
├── KVClient/                   # Linux gRPC client library
│   ├── src/
│   │   └── KVStoreGrpcClient.cpp
│   ├── include/
│   │   ├── AzureStorageKVStoreLibV2.h
│   │   └── KVTypes.h
│   ├── protos/
│   │   └── kvstore.proto
│   ├── CMakeLists.txt
│   └── README.md
└── KVPlayground/               # Linux test application
    ├── src/
    │   └── main.cpp
    ├── CMakeLists.txt
    └── README.md
```

### Testing

1. **Start Windows service**:
```powershell
cd KVService\build\Release
.\KVStoreServer.exe --port 8085 --log-level error --disable-metrics
```

2. **Run Linux client**:
```bash
export KVSTORE_GRPC_SERVER="windows-host:8085"
./build/KVPlayground/KVPlayground conversation_tokens.json 10 2
```

## Troubleshooting

### Connection refused
- Ensure KVService is running on Windows
- Check Windows Firewall allows the port:
  ```powershell
  New-NetFirewallRule -DisplayName "KVStore gRPC 8085" -Direction Inbound -Protocol TCP -LocalPort 8085 -Action Allow
  New-NetFirewallRule -DisplayName "KVStore gRPC 8086" -Direction Inbound -Protocol TCP -LocalPort 8086 -Action Allow
  ```
- Check Azure NSG rules allow inbound traffic on the ports
- Check Linux VM NSG allows outbound traffic on the ports
- Verify KVSTORE_GRPC_SERVER is set correctly

### NUMA node imbalance (one CPU at 100%, other idle)
On Windows VMs with 64+ cores (multiple NUMA nodes), a single process only uses one NUMA node.

**Solution**: Run multiple server instances pinned to each NUMA node:
```powershell
start "KVStore-Node0" /NODE 0 .\KVStoreServer.exe --port 8085 --log-level error --disable-metrics
start "KVStore-Node1" /NODE 1 .\KVStoreServer.exe --port 8086 --log-level error --disable-metrics
```

Check NUMA topology on Windows:
```powershell
# See processor groups
wmic cpu get NumberOfCores,NumberOfLogicalProcessors
# Or check Task Manager → Performance → CPU → Right-click → Change graph to → Logical processors
```

### Build errors on Linux
- Ensure all vcpkg packages are installed
- Check CMake finds vcpkg toolchain file
- Verify g++ version >= 7.0

### gRPC errors
- Check network connectivity between Linux and Windows
- Verify protocol buffer versions match
- Ensure both client and server are up-to-date

## License

See LICENSE file for details.

## Contributing

1. Fork the repository
2. Create your feature branch
3. Commit your changes
4. Push to the branch
5. Create a Pull Request
