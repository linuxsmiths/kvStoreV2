# KV Store gRPC Service - Quick Start Guide

This guide will help you get the KV Store gRPC service up and running quickly.

## Prerequisites

Before you begin, ensure you have:

1. **CMake 3.15+** installed
2. **Visual Studio 2019+** (Windows) or **GCC 7+/Clang 5+** (Linux)
3. **vcpkg** package manager set up
4. **Azure Storage account** with a container created

## Step 1: Install Dependencies

### Install gRPC and Protobuf via vcpkg

```powershell
# Windows
vcpkg install grpc:x64-windows-static protobuf:x64-windows-static

# Linux
vcpkg install grpc:x64-linux protobuf:x64-linux
```

## Step 2: Build the Service

### Windows

```powershell
# Navigate to your kvStore directory
cd c:\Users\kraman\source\kvStore

# Configure CMake (adjust vcpkg path as needed)
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/Users/kraman/source/repos/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release
```

### Linux

```bash
# Navigate to your kvStore directory
cd ~/kvStoreV2

# Configure CMake
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release
```

## Step 3: Start the Server

### Windows

```powershell
.\build\KVStoreService\Release\KVStoreServer.exe --log-level info
```

### Linux

```bash
./build/KVStoreService/KVStoreServer --log-level info --transport libcurl
```

You should see:
```
==================================================
KV Store gRPC Service
==================================================
Server listening on: 0.0.0.0:50051
Log Level: Information
HTTP Transport: WinHTTP
SDK Logging: Enabled
Multi-NIC: Disabled
==================================================
Press Ctrl+C to stop the server
```

## Step 4: Test with the Client

In a new terminal:

### Windows

```powershell
.\build\KVStoreService\Release\KVStoreClient.exe `
  --server localhost:50051 `
  --account "https://youraccount.blob.core.windows.net" `
  --container "yourcontainer"
```

### Linux

```bash
./build/KVStoreService/KVStoreClient \
  --server localhost:50051 \
  --account "https://youraccount.blob.core.windows.net" \
  --container "yourcontainer"
```

Expected output:
```
==================================================
KV Store gRPC Client - Test Application
==================================================
Server: localhost:50051
Account: https://youraccount.blob.core.windows.net
Container: yourcontainer
==================================================

[Test 1] Writing a test chunk...
  ✓ Write successful

[Test 2] Looking up cached tokens...
  ✓ Lookup successful
    Cached blocks: 1
    Last hash: 12345
    Locations: 1

[Test 3] Reading first cached block...
  ✓ Read successful
    Chunk hash: 12345
    Buffer size: 5
    Tokens: 5

==================================================
Test completed
==================================================
```

## Step 5: Integrate with Your Application

### Option A: C++ Client

```cpp
#include "kvstore.grpc.pb.h"
#include <grpcpp/grpcpp.h>

int main() {
    // Connect to service
    auto channel = grpc::CreateChannel(
        "localhost:50051",
        grpc::InsecureChannelCredentials()
    );
    auto stub = kvstore::KVStoreService::NewStub(channel);
    
    // Create a lookup request
    kvstore::LookupRequest request;
    request.set_account_url("https://account.blob.core.windows.net");
    request.set_container_name("container");
    request.set_partition_key("mykey");
    request.set_completion_id("run-001");
    
    // Add tokens
    for (int64_t token : {100, 200, 300, 400, 500}) {
        request.add_tokens(token);
    }
    
    // Call service
    kvstore::LookupResponse response;
    grpc::ClientContext context;
    grpc::Status status = stub->Lookup(&context, request, &response);
    
    if (status.ok() && response.success()) {
        std::cout << "Found " << response.cached_blocks() 
                  << " cached blocks" << std::endl;
        
        // Read each block
        for (const auto& location : response.locations()) {
            kvstore::ReadRequest readReq;
            readReq.set_account_url(request.account_url());
            readReq.set_container_name(request.container_name());
            readReq.set_location(location.location());
            
            kvstore::ReadResponse readResp;
            grpc::ClientContext readCtx;
            
            if (stub->Read(&readCtx, readReq, &readResp).ok()) {
                // Process chunk
                auto& chunk = readResp.chunk();
                std::cout << "Read chunk with " 
                          << chunk.tokens_size() << " tokens" << std::endl;
            }
        }
    }
    
    return 0;
}
```

### Option B: Python Client

First, generate Python client code:

```bash
cd KVStoreService/protos
python -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. kvstore.proto
```

Then use it:

```python
import grpc
import kvstore_pb2
import kvstore_pb2_grpc

def main():
    # Connect to service
    channel = grpc.insecure_channel('localhost:50051')
    stub = kvstore_pb2_grpc.KVStoreServiceStub(channel)
    
    # Lookup request
    request = kvstore_pb2.LookupRequest(
        account_url='https://account.blob.core.windows.net',
        container_name='container',
        partition_key='mykey',
        completion_id='run-001',
        tokens=[100, 200, 300, 400, 500]
    )
    
    response = stub.Lookup(request)
    
    if response.success:
        print(f"Found {response.cached_blocks} cached blocks")
        
        # Read each block
        for location in response.locations:
            read_req = kvstore_pb2.ReadRequest(
                account_url=request.account_url,
                container_name=request.container_name,
                location=location.location
            )
            
            read_resp = stub.Read(read_req)
            if read_resp.success and read_resp.found:
                chunk = read_resp.chunk
                print(f"Read chunk with {len(chunk.tokens)} tokens")

if __name__ == '__main__':
    main()
```

### Option C: .NET Client (C#)

Add NuGet packages:
```bash
dotnet add package Grpc.Net.Client
dotnet add package Google.Protobuf
dotnet add package Grpc.Tools
```

Generate code from proto:
```xml
<!-- Add to .csproj -->
<ItemGroup>
  <Protobuf Include="..\KVStoreService\protos\kvstore.proto" 
            GrpcServices="Client" />
</ItemGroup>
```

Use the client:
```csharp
using Grpc.Net.Client;
using Kvstore;

var channel = GrpcChannel.ForAddress("http://localhost:50051");
var client = new KVStoreService.KVStoreServiceClient(channel);

var request = new LookupRequest
{
    AccountUrl = "https://account.blob.core.windows.net",
    ContainerName = "container",
    PartitionKey = "mykey",
    CompletionId = "run-001",
    Tokens = { 100, 200, 300, 400, 500 }
};

var response = await client.LookupAsync(request);

if (response.Success)
{
    Console.WriteLine($"Found {response.CachedBlocks} cached blocks");
    
    foreach (var location in response.Locations)
    {
        var readReq = new ReadRequest
        {
            AccountUrl = request.AccountUrl,
            ContainerName = request.ContainerName,
            Location = location.Location
        };
        
        var readResp = await client.ReadAsync(readReq);
        if (readResp.Success && readResp.Found)
        {
            Console.WriteLine($"Read chunk with {readResp.Chunk.Tokens.Count} tokens");
        }
    }
}
```

## Common Configuration Options

### Production Server

```bash
# High-performance production setup
./KVStoreServer \
  --port 443 \
  --log-level error \
  --disable-sdk-logging \
  --transport winhttp
```

### Development Server with Verbose Logging

```bash
# Debug with detailed logs
./KVStoreServer \
  --port 50051 \
  --log-level verbose \
  --transport libcurl
```

### Multi-NIC Enabled (if using custom Azure SDK build)

```bash
./KVStoreServer \
  --port 50051 \
  --enable-multi-nic \
  --log-level info
```

## Authentication Setup (Future)

Currently the service uses insecure credentials. For production, you'll need to:

1. **Enable TLS**: Configure SSL certificates
2. **Add Auth Header**: Pass Azure AD tokens
3. **Validate Tokens**: Server-side token validation

Example future usage:
```cpp
// Client with auth
auto creds = grpc::SslCredentials(grpc::SslCredentialsOptions());
auto channel = grpc::CreateChannel("secure-server:443", creds);

// Add auth metadata
grpc::ClientContext context;
context.AddMetadata("authorization", "Bearer YOUR_TOKEN");
```

## Troubleshooting

### "Server failed to start"
- Port 50051 already in use? Try `--port 50052`
- Missing dependencies? Run `vcpkg install grpc protobuf`

### "Failed to initialize storage"
- Check Azure Storage account URL is correct
- Verify managed identity or credentials are set up
- Ensure container exists

### "Connection refused"
- Is server running? Check with `netstat -an | grep 50051`
- Firewall blocking port? Add exception
- Wrong hostname? Try `127.0.0.1` instead of `localhost`

## Next Steps

1. **Load Testing**: Use tools like ghz to benchmark performance
2. **Monitoring**: Add Application Insights or Prometheus metrics
3. **Security**: Implement TLS and authentication
4. **Service Fabric**: Package as SF service for Azure deployment

## Support

For issues or questions:
- Check the main README.md for detailed documentation
- Review server logs with `--log-level verbose`
- Test with the provided client first to isolate issues
