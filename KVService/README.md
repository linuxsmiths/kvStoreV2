# KV Store gRPC Service

High-performance, multi-tenant gRPC service for KV (Key-Value) cache offload in large language model inference scenarios. Part of the **KV Store Service** platform that runs in Azure Platform Subscription.

## Overview

The KVService provides:
- **Unified Endpoint**: Single gRPC service handles multiple customer storage accounts
- **Account Resolution**: Resolves resource names to Azure Storage connections
- **High Performance**: NUMA-aware, multi-NIC, async gRPC with streaming
- **Multi-Tenant**: Serves multiple customers from shared infrastructure

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Windows VM (FX96mds_v2)                      │
│                                                                 │
│  ┌──────────────────────────┐  ┌──────────────────────────┐    │
│  │   KVStoreServer :8085    │  │   KVStoreServer :8086    │    │
│  │   (NUMA Node 0)          │  │   (NUMA Node 1)          │    │
│  │                          │  │                          │    │
│  │  ┌────────────────────┐  │  │  ┌────────────────────┐  │    │
│  │  │  gRPC Async Server │  │  │  │  gRPC Async Server │  │    │
│  │  │  (Callback API)    │  │  │  │  (Callback API)    │  │    │
│  │  └─────────┬──────────┘  │  │  └─────────┬──────────┘  │    │
│  │            │             │  │            │             │    │
│  │  ┌─────────▼──────────┐  │  │  ┌─────────▼──────────┐  │    │
│  │  │ KVStoreServiceImpl │  │  │  │ KVStoreServiceImpl │  │    │
│  │  │ • AccountResolver  │  │  │  │ • AccountResolver  │  │    │
│  │  │ • Reactor Pool     │  │  │  │ • Reactor Pool     │  │    │
│  │  └─────────┬──────────┘  │  │  └─────────┬──────────┘  │    │
│  │            │             │  │            │             │    │
│  │  ┌─────────▼──────────┐  │  │  ┌─────────▼──────────┐  │    │
│  │  │ AzureStorageKV     │  │  │  │ AzureStorageKV     │  │    │
│  │  │ StoreLibV2         │  │  │  │ StoreLibV2         │  │    │
│  │  └────────────────────┘  │  │  └────────────────────┘  │    │
│  └──────────────────────────┘  └──────────────────────────┘    │
│                                                                 │
│  Multi-NIC Round Robin → Azure Blob Storage                    │
└─────────────────────────────────────────────────────────────────┘
```

## Quick Start

### Prerequisites

- **Windows Server 2019+** or Windows 11
- **Visual Studio 2022** with C++ workload
- **vcpkg** package manager
- **Azure Storage Account** for testing

### Build

```powershell
cd KVService

# Build with local Azure SDK (includes multi-NIC support)
.\build_with_local_sdk.ps1

# Or build with official Azure SDK
.\build_with_official_sdk.ps1
```

### Run (Single Process)

```powershell
.\build\Release\KVStoreServer.exe --port 8085 --log-level info
```

### Run (Multi-NUMA - Recommended for 64+ core VMs)

For VMs with multiple NUMA nodes (e.g., FX96mds_v2 with 96 cores, 2 NUMA nodes):

```powershell
# Start two server processes, one per NUMA node
start "KVStore-Node0" /NODE 0 .\build\Release\KVStoreServer.exe --port 8085 --log-level error --disable-metrics
start "KVStore-Node1" /NODE 1 .\build\Release\KVStoreServer.exe --port 8086 --log-level error --disable-metrics
```

This ensures:
- Full CPU utilization across all cores
- No cross-NUMA memory access penalty
- 2× throughput compared to single process

## Project Structure

```
KVService/
├── protos/
│   └── kvstore.proto              # gRPC service definition
├── include/
│   ├── KVStoreServiceImpl.h       # Service interface
│   ├── IAccountResolver.h         # Account resolution interface
│   ├── InMemoryAccountResolver.h  # In-memory resolver implementation
│   ├── AzureStorageKVStoreLibV2.h # KV Store library interface
│   ├── KVTypes.h                  # Type definitions
│   └── MetricsHelper.h            # Metrics utilities
├── src/
│   ├── server.cpp                 # Server entry point
│   ├── KVStoreServiceImpl.cpp     # Service implementation
│   ├── InMemoryAccountResolver.cpp# Resolver implementation
│   ├── AzureStorageKVStoreLibV2.cpp # KV Store library
│   ├── MetricsHelper.cpp          # Metrics implementation
│   └── reactors/                  # gRPC async reactors
│       ├── LookupReactor.cpp      # Lookup RPC handler
│       ├── ReadReactor.cpp        # Read RPC handler
│       ├── WriteReactor.cpp       # Write RPC handler
│       └── StreamingReadReactor.cpp # Streaming read handler
├── build_with_local_sdk.ps1       # Build script (multi-NIC SDK)
├── build_with_official_sdk.ps1    # Build script (official SDK)
├── CMakeLists.txt                 # Build configuration
├── vcpkg.json                     # Package dependencies
└── README.md                      # This file
```

## Command Line Options

```
KVStoreServer [options]

Options:
  --port PORT              gRPC listen port (default: 50051)
  --host HOST              Bind address (default: 0.0.0.0)
  --threads NUM            Worker threads (default: auto-detect)
  --blob-dns-suffix SUFFIX Azure blob DNS suffix (default: .blob.core.windows.net)
  --log-level LEVEL        Log level: error, info, verbose (default: info)
  --transport TYPE         HTTP transport: winhttp, libcurl (default: libcurl)
  --disable-metrics        Disable console metrics display
  --enable-sdk-logging     Enable Azure SDK debug logging
  --disable-multi-nic      Disable multi-NIC round-robin
```

## gRPC API

The service exposes four RPC methods:

### Lookup
```protobuf
rpc Lookup(LookupRequest) returns (LookupResponse);
```
Finds cached token blocks matching a sequence. Uses Bloom filter for O(1) cache miss detection.

### Read
```protobuf
rpc Read(ReadRequest) returns (ReadResponse);
```
Retrieves a cached chunk by blob location.

### Write
```protobuf
rpc Write(WriteRequest) returns (WriteResponse);
```
Stores a new chunk to Azure Blob Storage.

### StreamingRead
```protobuf
rpc StreamingRead(stream ReadRequest) returns (stream ReadResponse);
```
Bidirectional streaming for batch reads with reduced latency. Parallel storage access on server side.

## Account Resolution

The service uses `IAccountResolver` interface to resolve resource names to storage connections:

```cpp
// Client sends: resource_name = "mystorageaccount"
// Server resolves to: https://mystorageaccount.blob.core.windows.net
```

### InMemoryAccountResolver (Current)
Appends configured blob DNS suffix to resource name.

### DatabaseAccountResolver (Future)
Will support:
- Customer account database lookup
- Cross-region routing
- Access control and authorization

## Performance Optimizations

### NUMA-Aware Deployment
- Run 2 processes on multi-NUMA VMs
- Pin each process to a NUMA node with `start /NODE N`
- Eliminates cross-NUMA memory access latency

### gRPC Settings
| Setting | Value | Purpose |
|---------|-------|---------|
| TCP_NODELAY | 1 | Disable Nagle's algorithm |
| KEEPALIVE_TIME | 10s | Maintain warm connections |
| MAX_CONCURRENT_STREAMS | 200 | High parallelism |
| MAX_FRAME_SIZE | 16MB | Reduce framing overhead |
| STREAM_WINDOW | 64MB | Large flow control window |

### Azure Storage Settings
| Setting | Value | Purpose |
|---------|-------|---------|
| Connection Pool | 100 | Concurrent HTTP connections |
| DNS Cache TTL | 300s | Reduce DNS lookups |
| SSL Session Reuse | Enabled | Faster TLS handshakes |
| Multi-NIC | Round-robin | Aggregate bandwidth |

## Deployment

### Azure VM Setup

1. **Create Windows VM** (FX96mds_v2 recommended for high throughput)
2. **Configure NSG Rules**:
   ```powershell
   # Allow gRPC ports
   New-NetFirewallRule -DisplayName "KVStore 8085" -Direction Inbound -Protocol TCP -LocalPort 8085 -Action Allow
   New-NetFirewallRule -DisplayName "KVStore 8086" -Direction Inbound -Protocol TCP -LocalPort 8086 -Action Allow
   ```
3. **Deploy and run**:
   ```powershell
   .\deploy_server.ps1
   ```

### Multi-Cluster Deployment

For production, deploy as part of KV Store Service:
- Multiple VMs per cluster (array of Windows VMs)
- Multiple clusters per region
- Multiple regions globally

See [ARCHITECTURE.md](../docs/ARCHITECTURE.md) for full deployment topology.

## Troubleshooting

### Build Fails - "Could not find gRPC"
```powershell
# Ensure vcpkg is configured
vcpkg install grpc protobuf --triplet=x64-windows-static
```

### Server won't start - Port in use
```powershell
# Check what's using the port
netstat -ano | findstr :8085
# Kill the process or use a different port
```

### NUMA node imbalance
If one CPU is at 100% while others are idle:
```powershell
# Run multi-process mode
start "KVStore-Node0" /NODE 0 .\KVStoreServer.exe --port 8085
start "KVStore-Node1" /NODE 1 .\KVStoreServer.exe --port 8086
```

### Connection timeout from clients
- Verify firewall rules allow inbound traffic
- Check Azure NSG rules for the ports
- Ensure Private Link is configured if cross-subscription

## Dependencies

Managed via vcpkg.json:
- `grpc` - gRPC framework with HTTP/2
- `protobuf` - Protocol Buffers serialization
- `azure-storage-blobs-cpp` - Azure Blob Storage SDK
- `azure-identity-cpp` - Azure authentication (DefaultAzureCredential)
- `azure-core-cpp` - Azure core libraries
- `nlohmann-json` - JSON parsing

## Related Documentation

- [ARCHITECTURE.md](../docs/ARCHITECTURE.md) - System architecture and deployment
- [QUICKSTART.md](QUICKSTART.md) - Detailed setup guide
- [KVClient README](../KVClient/README.md) - Linux client library

## License

See LICENSE file in root directory.
