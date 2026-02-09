#include "AzureStorageKVStoreLibV2.h"
#include <future>
#include <azure/storage/blobs.hpp>
#include <azure/identity/default_azure_credential.hpp>
#include <azure/core/io/body_stream.hpp>
#include <azure/core/http/curl_transport.hpp>
#include <azure/core/diagnostics/logger.hpp>
#ifdef _WIN32
#include <azure/core/http/win_http_transport.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#endif
#include <cassert>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <mutex>
#include <atomic>
#include <curl/curl.h>

// Build flavor detection: USE_LOCAL_AZURE_SDK enables custom features for local SDK builds
// Define this macro when building against locally-modified Azure SDK with custom curl options
#ifndef USE_LOCAL_AZURE_SDK
#define USE_LOCAL_AZURE_SDK 0  // Default: build against official Azure SDK
#endif

// Multi-NIC support - only available with local Azure SDK (requires custom curl transport modifications)
#if USE_LOCAL_AZURE_SDK
static std::vector<std::string> g_nicIPs;
static std::atomic<size_t> g_currentIndex{0};
#endif

#if USE_LOCAL_AZURE_SDK
static std::vector<std::string> DiscoverNetworkInterfaces() {
    std::vector<std::string> interfaces;
#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return interfaces;
    }

    ULONG bufferSize = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = nullptr;
    ULONG iterations = 0;
    DWORD result;

    do {
        pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(bufferSize);
        if (pAddresses == nullptr) {
            WSACleanup();
            return interfaces;
        }

        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, pAddresses, &bufferSize);
        
        if (result == ERROR_BUFFER_OVERFLOW) {
            free(pAddresses);
            pAddresses = nullptr;
        } else {
            break;
        }
        iterations++;
    } while ((result == ERROR_BUFFER_OVERFLOW) && (iterations < 3));

    if (result == NO_ERROR) {
        PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses;
        while (pCurrAddresses) {
            if (pCurrAddresses->OperStatus == IfOperStatusUp) {
                PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurrAddresses->FirstUnicastAddress;
                while (pUnicast) {
                    SOCKADDR* sa = pUnicast->Address.lpSockaddr;
                    
                    if (sa->sa_family == AF_INET) {
                        char ipStr[INET_ADDRSTRLEN];
                        SOCKADDR_IN* sa_in = (SOCKADDR_IN*)sa;
                        inet_ntop(AF_INET, &(sa_in->sin_addr), ipStr, INET_ADDRSTRLEN);
                        
                        std::string ipAddress(ipStr);
                        if (ipAddress != "127.0.0.1") {
                            interfaces.push_back(ipAddress);
                        }
                    }
                    pUnicast = pUnicast->Next;
                }
            }
            pCurrAddresses = pCurrAddresses->Next;
        }
    }

    if (pAddresses) {
        free(pAddresses);
    }
    WSACleanup();
#else
    // Linux implementation using getifaddrs
    struct ifaddrs *ifaddr, *ifa;
    
    if (getifaddrs(&ifaddr) == -1) {
        std::cerr << "[Multi-NIC] Failed to get network interfaces" << std::endl;
        return interfaces;
    }
    
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        
        // Only interested in IPv4 addresses
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(sa->sin_addr), ipStr, INET_ADDRSTRLEN);
            
            std::string ipAddress(ipStr);
            // Skip loopback
            if (ipAddress != "127.0.0.1") {
                std::cout << "[Multi-NIC] Found interface: " << ifa->ifa_name << " -> " << ipAddress << std::endl;
                interfaces.push_back(ipAddress);
            }
        }
    }
    
    freeifaddrs(ifaddr);
#endif
    return interfaces;
}

// Socket callback to bind to specific source IP
static curl_socket_t MultiNicSocketCallback(void* clientp, curlsocktype purpose, struct curl_sockaddr* address) {
    std::cout << "[Multi-NIC Callback] Called - purpose: " << purpose << ", g_nicIPs size: " << g_nicIPs.size() << std::endl;
    std::cout.flush();
    
    if (purpose != CURLSOCKTYPE_IPCXN || g_nicIPs.empty()) {
        std::cout << "[Multi-NIC Callback] Using default socket (purpose=" << purpose << ", empty=" << g_nicIPs.empty() << ")" << std::endl;
        std::cout.flush();
        return socket(address->family, address->socktype, address->protocol);
    }
    
    // Round-robin selection with fallback to try all interfaces
    size_t numNics = g_nicIPs.size();
    size_t startIndex = g_currentIndex.load(std::memory_order_relaxed);
    size_t next = (startIndex + 1 < numNics) ? startIndex + 1 : 0;
    
    // Atomically increment for next caller
    size_t current = startIndex;
    while (!g_currentIndex.compare_exchange_weak(current, next,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
        next = (current + 1 < numNics) ? current + 1 : 0;
    }
    
    // Try to bind to interfaces, starting with the selected one
    for (size_t attempt = 0; attempt < numNics; attempt++) {
        size_t tryIndex = (startIndex + attempt) % numNics;
        
        // Create socket
        curl_socket_t sockfd = socket(address->family, address->socktype, address->protocol);
        if (sockfd == CURL_SOCKET_BAD) {
            std::cerr << "[Multi-NIC Callback] Failed to create socket for " << g_nicIPs[tryIndex] << std::endl;
            std::cerr.flush();
            continue;
        }
        
        // Bind to specific source IP
        struct sockaddr_in bind_addr = {};
        bind_addr.sin_family = AF_INET;
#ifdef _WIN32
        bind_addr.sin_addr.s_addr = inet_addr(g_nicIPs[tryIndex].c_str());
#else
        inet_pton(AF_INET, g_nicIPs[tryIndex].c_str(), &bind_addr.sin_addr);
#endif
        bind_addr.sin_port = 0; // Let OS choose port
        
        if (attempt == 0) {
            std::cout << "[Multi-NIC Callback] Binding socket to " << g_nicIPs[tryIndex] << " (index " << tryIndex << ")" << std::endl;
            std::cout.flush();
        }
        
        if (bind(sockfd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) == 0) {
            if (attempt > 0) {
                std::cout << "[Multi-NIC Callback] Successfully bound to " << g_nicIPs[tryIndex] << " on retry " << attempt << std::endl;
                std::cout.flush();
            }
            return sockfd;
        }
        
        // Bind failed, try next interface
#ifdef _WIN32
        int err = WSAGetLastError();
        if (attempt == 0) {
            std::cerr << "[Multi-NIC Callback] Bind failed for " << g_nicIPs[tryIndex] << " error: " << err << ", trying next..." << std::endl;
        }
        closesocket(sockfd);
#else
        int err = errno;
        if (attempt == 0) {
            std::cerr << "[Multi-NIC Callback] Bind failed for " << g_nicIPs[tryIndex] << " error: " << err << " (" << strerror(err) << "), trying next..." << std::endl;
            std::cerr.flush();
        }
        close(sockfd);
#endif
    }
    
    std::cerr << "[Multi-NIC Callback] All interfaces failed, falling back to default" << std::endl;
    std::cerr.flush();
    return socket(address->family, address->socktype, address->protocol);
}

// Multi-NIC curl callback - sets socket callback for binding
static void MultiNicCurlCallback(void* curlHandle) {
    if (!curlHandle || g_nicIPs.empty()) return;
    
    // Use socket callback to bind to source IP (more reliable than CURLOPT_INTERFACE on Windows)
    curl_easy_setopt(static_cast<CURL*>(curlHandle), CURLOPT_OPENSOCKETFUNCTION, MultiNicSocketCallback);
    curl_easy_setopt(static_cast<CURL*>(curlHandle), CURLOPT_OPENSOCKETDATA, nullptr);
}
#endif  // USE_LOCAL_AZURE_SDK

// Simple JSON parser for additionalVersions metadata
// Format: [{"hash":"123","parentHash":"456","location":"guid"},...]
struct AdditionalVersion {
    hash_t hash;
    hash_t parentHash;
    std::string location;
};

std::vector<AdditionalVersion> ParseAdditionalVersions(const std::string& jsonStr) {
    std::vector<AdditionalVersion> versions;
    if (jsonStr.empty() || jsonStr == "[]") {
        return versions;
    }
    
    // Simple parser: split by "},{"
    size_t pos = 0;
    while (pos < jsonStr.length()) {
        // Find next object
        size_t start = jsonStr.find("{\"hash\":", pos);
        if (start == std::string::npos) break;
        
        size_t end = jsonStr.find("}", start);
        if (end == std::string::npos) break;
        
        std::string obj = jsonStr.substr(start, end - start + 1);
        
        // Extract hash
        size_t hashStart = obj.find("\"hash\":\"") + 8;
        size_t hashEnd = obj.find("\"", hashStart);
        std::string hashStr = obj.substr(hashStart, hashEnd - hashStart);
        
        // Extract parentHash
        size_t parentStart = obj.find("\"parentHash\":\"") + 14;
        size_t parentEnd = obj.find("\"", parentStart);
        std::string parentStr = obj.substr(parentStart, parentEnd - parentStart);
        
        // Extract location
        size_t locStart = obj.find("\"location\":\"") + 12;
        size_t locEnd = obj.find("\"", locStart);
        std::string location = obj.substr(locStart, locEnd - locStart);
        
        AdditionalVersion version;
        version.hash = std::stoull(hashStr);
        version.parentHash = std::stoull(parentStr);
        version.location = location;
        versions.push_back(version);
        
        pos = end + 1;
    }
    
    return versions;
}

// Generate a simple GUID-like string for versioned blobs
std::string GenerateGuid() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    
    uint64_t part1 = dis(gen);
    uint64_t part2 = dis(gen);
    
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(16) << part1 << "-" << std::setw(16) << part2;
    return ss.str();
}

// Serialize additionalVersions array to JSON string
std::string SerializeAdditionalVersions(const std::vector<AdditionalVersion>& versions) {
    if (versions.empty()) {
        return "[]";
    }
    
    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < versions.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"hash\":\"" << versions[i].hash << "\""
           << ",\"parentHash\":\"" << versions[i].parentHash << "\""
           << ",\"location\":\"" << versions[i].location << "\"}";
    }
    ss << "]";
    return ss.str();
}

bool AzureStorageKVStoreLibV2::Initialize(const std::string& accountUrl, const std::string& containerName, HttpTransportProtocol transport, bool enableSdkLogging, bool enableMultiNic) {
    azureAccountUrl_ = accountUrl;
    azureContainerName_ = containerName;

    try {
        // Initialize multi-NIC support if enabled (only available with local Azure SDK)
#if USE_LOCAL_AZURE_SDK
        if (enableMultiNic) {
            g_nicIPs = DiscoverNetworkInterfaces();
            if (!g_nicIPs.empty()) {
                std::cout << "[Multi-NIC] Discovered " << g_nicIPs.size() << " network interfaces:\n";
                for (size_t i = 0; i < g_nicIPs.size(); i++) {
                    std::cout << "[Multi-NIC]   [" << i << "] " << g_nicIPs[i] << "\n";
                }
                std::cout << "[Multi-NIC] Multi-NIC load balancing ENABLED\n";
            } else {
                std::cout << "[Multi-NIC] Warning: No non-loopback interfaces found\n";
            }
        } else {
            std::cout << "[Multi-NIC] Multi-NIC load balancing DISABLED (use --multi-nic to enable)\n";
        }
#else
        if (enableMultiNic) {
            std::cout << "[Multi-NIC] WARNING: Multi-NIC support not available (requires local Azure SDK build)\n";
        }
#endif

        // Conditionally enable Azure SDK verbose logging to see connection pool behavior
        if (enableSdkLogging) {
            Azure::Core::Diagnostics::Logger::SetLevel(Azure::Core::Diagnostics::Logger::Level::Verbose);
            Azure::Core::Diagnostics::Logger::SetListener([](Azure::Core::Diagnostics::Logger::Level level, std::string const& message) {
                std::cout << "[Azure SDK] " << message << std::endl;
            });
        }
        
        auto credential = std::make_shared<Azure::Identity::DefaultAzureCredential>();
        
        // Configure transport protocol with optimizations
        Azure::Storage::Blobs::BlobClientOptions clientOptions;
        
        if (transport == HttpTransportProtocol::LibCurl) {
#ifdef AZ_PLATFORM_WINDOWS
            // On Windows, explicitly use libcurl transport with performance optimizations
            Azure::Core::Http::CurlTransportOptions curlOptions;
            
#if USE_LOCAL_AZURE_SDK
            // ===== LOCAL SDK FEATURES: Advanced curl options =====
            // Connection Pooling & Reuse
            curlOptions.HttpKeepAlive = true;
            curlOptions.EnableCurlSslCaching = true;
            curlOptions.MaxConnectionsCache = 100;  // Allow 100 concurrent connections for parallel reads
            curlOptions.DnsCacheTimeout = 300;
#endif
            
            // ===== Timeout Settings =====
            // Revert to 3000ms - ConnectionTimeout is for TCP connection establishment, not pool cleanup
            curlOptions.ConnectionTimeout = std::chrono::milliseconds(3000);
            
            // ===== SSL/TLS Optimizations =====
            // Disable certificate revocation list check to eliminate CRL fetch latency
            curlOptions.SslOptions.EnableCertificateRevocationListCheck = false;
            
            // ===== Multi-NIC Support (Local Azure SDK only) =====
            // Set curl callback to round-robin across network interfaces if enabled
#if USE_LOCAL_AZURE_SDK
            if (enableMultiNic && !g_nicIPs.empty()) {
                std::cout << "[Multi-NIC] Setting CurlOptionsCallback for round-robin interface selection\n";
                curlOptions.CurlOptionsCallback = MultiNicCurlCallback;
            } else if (enableMultiNic) {
                std::cout << "[Multi-NIC] WARNING: Multi-NIC requested but no interfaces discovered!\n";
            }
#endif
            
            // Allow SSL connections even if CRL retrieval fails (prevents blocking)
            curlOptions.SslOptions.AllowFailedCrlRetrieval = true;
            
            // ===== Signal Handling =====
            // Disable signal handlers for better multi-threaded performance
            // Prevents signals from interfering with concurrent operations
            curlOptions.NoSignal = true;
            
            // ===== Proxy Settings =====
            // Explicitly disable proxy to avoid environment-based proxy lookups
            curlOptions.Proxy = "";  // Empty string = no proxy
            
            clientOptions.Transport.Transport = std::make_shared<Azure::Core::Http::CurlTransport>(curlOptions);
            Log(LogLevel::Information, "[KVStore V2] Using libcurl HTTP transport (optimized: MaxConnections=100, DNS cache=300s)");
#else
            // On non-Windows (Linux), configure curl options with multi-NIC support
            Azure::Core::Http::CurlTransportOptions curlOptions;
            
            curlOptions.ConnectionTimeout = std::chrono::milliseconds(3000);
            curlOptions.SslOptions.EnableCertificateRevocationListCheck = false;
            curlOptions.SslOptions.AllowFailedCrlRetrieval = true;
            curlOptions.NoSignal = true;
            curlOptions.Proxy = "";
            
#if USE_LOCAL_AZURE_SDK
            // Connection pooling optimizations
            curlOptions.HttpKeepAlive = true;
            curlOptions.MaxConnectionsCache = 100;
            curlOptions.DnsCacheTimeout = 300;  // seconds as long
            curlOptions.EnableCurlSslCaching = true;
            
            // Multi-NIC Support - set callback to bind to different interfaces
            if (enableMultiNic && !g_nicIPs.empty()) {
                std::cout << "[Multi-NIC] Setting CurlOptionsCallback for round-robin interface selection\n";
                std::cout.flush();
                curlOptions.CurlOptionsCallback = MultiNicCurlCallback;
            } else if (enableMultiNic) {
                std::cout << "[Multi-NIC] WARNING: Multi-NIC requested but no interfaces discovered!\n";
                std::cout.flush();
            }
#endif
            
            clientOptions.Transport.Transport = std::make_shared<Azure::Core::Http::CurlTransport>(curlOptions);
            Log(LogLevel::Information, "[KVStore V2] Using libcurl HTTP transport with multi-NIC support (Linux)");
#endif
        } else {
#ifdef USE_CURL_TRANSPORT
            // Use CurlTransport when building with curl-based Azure SDK
            Azure::Core::Http::CurlTransportOptions curlOptions;
            curlOptions.ConnectionTimeout = std::chrono::milliseconds(3000);
            curlOptions.Proxy = "";  // Empty string = no proxy
            clientOptions.Transport.Transport = std::make_shared<Azure::Core::Http::CurlTransport>(curlOptions);
            Log(LogLevel::Information, "[KVStore V2] Using libcurl HTTP transport (curl-based SDK)");
#elif defined(AZ_PLATFORM_WINDOWS)
            // On Windows, explicitly use WinHTTP transport
            clientOptions.Transport.Transport = std::make_shared<Azure::Core::Http::WinHttpTransport>();
            Log(LogLevel::Information, "[KVStore V2] Using WinHTTP transport (native Windows)");
#else
            Log(LogLevel::Information, "[KVStore V2] WinHTTP not available on this platform, using default transport");
#endif
        }
        
        // Configure aggressive retry policy to minimize overhead
        clientOptions.Retry.MaxRetries = 2;  // Reduce retries for faster failure
        clientOptions.Retry.RetryDelay = std::chrono::milliseconds(50);  // Faster retry intervals
        clientOptions.Retry.MaxRetryDelay = std::chrono::milliseconds(1000);  // Lower max delay
        
        // Disable telemetry for performance
        clientOptions.Telemetry.ApplicationId = "";
        
        blobContainerClient_ = std::make_shared<Azure::Storage::Blobs::BlobContainerClient>(
            accountUrl + "/" + containerName, credential, clientOptions);
        
        Log(LogLevel::Information, "[KVStore V2] Initialized with Azure account: " + accountUrl + ", container: " + containerName);
        return true;
    }
    catch (const std::exception& e) {
        Log(LogLevel::Error, "[KVStore V2] Failed to initialize: " + std::string(e.what()));
        return false;
    }
}

// V2 API: Lookup - Returns block locations for multi-version support
template<typename TokenIterator>
LookupResult AzureStorageKVStoreLibV2::Lookup(
    const std::string& partitionKey,
    const std::string& completionId,
    TokenIterator begin,
    TokenIterator end,
    const std::vector<hash_t>& precomputedHashes
) const {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    constexpr size_t blockSize = 128;
    
    // Only process full blocks
    size_t totalTokens = std::distance(begin, end);
    size_t numFullBlocks = totalTokens / blockSize;
    
    if (numFullBlocks == 0) {
        return LookupResult(0, 0);
    }
    
    Log(LogLevel::Verbose, "[KVStore V2 Lookup] Starting lookup for " + std::to_string(numFullBlocks) + " blocks (with locations)", completionId);
    
    // Prepare block information
    struct BlockInfo {
        std::string blobName;
        hash_t expectedHash;
    };
    std::vector<BlockInfo> blocks(numFullBlocks);
    
    // Prepare all block blob names first
    for (size_t i = 0; i < numFullBlocks; ++i) {
        auto blockBegin = begin + (i * blockSize);
        auto blockEnd = blockBegin + blockSize;
        blocks[i].blobName = EncodeTokensToBlobName(blockBegin, blockEnd);
        blocks[i].expectedHash = (i < precomputedHashes.size()) ? precomputedHashes[i] : 0;
    }
    
    // Launch all GetProperties calls in parallel
    std::vector<std::future<std::tuple<bool, hash_t, hash_t, std::string, std::string>>> futures;
    for (const auto& block : blocks) {
        futures.push_back(std::async(std::launch::async, [this, block]() -> std::tuple<bool, hash_t, hash_t, std::string, std::string> {
            try {
                auto blobClient = blobContainerClient_->GetBlobClient(block.blobName);
                auto properties = blobClient.GetProperties();
                
                hash_t storedHash = 0;
                hash_t parentHash = 0;
                std::string multiVersion = "";
                std::string additionalVersions = "";
                
                auto hashIt = properties.Value.Metadata.find("hash");
                if (hashIt != properties.Value.Metadata.end()) {
                    storedHash = std::stoull(hashIt->second);
                }
                
                auto parentIt = properties.Value.Metadata.find("parenthash");
                if (parentIt != properties.Value.Metadata.end()) {
                    parentHash = std::stoull(parentIt->second);
                }
                
                auto multiVerIt = properties.Value.Metadata.find("multiversion");
                if (multiVerIt != properties.Value.Metadata.end()) {
                    multiVersion = multiVerIt->second;
                }
                
                auto additionalIt = properties.Value.Metadata.find("additionalversions");
                if (additionalIt != properties.Value.Metadata.end()) {
                    additionalVersions = additionalIt->second;
                }
                
                return {true, storedHash, parentHash, multiVersion, additionalVersions};
            } catch (const Azure::Storage::StorageException& ex) {
                return {false, 0, 0, "", ""};
            }
        }));
    }
    
    // Process results sequentially to validate chain
    LookupResult result;
    hash_t expectedParentHash = 0;
    
    for (size_t blockNum = 0; blockNum < numFullBlocks; ++blockNum) {
        auto [found, storedHash, parentHash, multiVersion, additionalVersions] = futures[blockNum].get();
        
        if (!found) {
            Log(LogLevel::Error, "[KVStore V2 Lookup]   ✗ Block " + std::to_string(blockNum) + " not found, breaking chain", completionId);
            break;
        }
        
        std::string locationToRead = blocks[blockNum].blobName;  // Default to token-based blob name
        hash_t blockHash = storedHash;
        
        // Debug: Log metadata values
        Log(LogLevel::Verbose, "[KVStore V2 Lookup]   Block " + std::to_string(blockNum) + " metadata: multiVersion='" + multiVersion + 
            "', additionalVersions.size=" + std::to_string(additionalVersions.size()) + 
            ", parentHash=" + std::to_string(parentHash) + ", expectedParent=" + std::to_string(expectedParentHash), completionId);
        
        // Check if this is a multi-version blob (simplified: just check if additionalVersions exists)
        if (!additionalVersions.empty()) {
            Log(LogLevel::Verbose, "[KVStore V2 Lookup]   Multi-version blob detected for block " + std::to_string(blockNum), completionId);
            
            // Check default version first
            if (blockNum == 0 || parentHash == expectedParentHash) {
                // Default version matches, use default location
                Log(LogLevel::Verbose, "[KVStore V2 Lookup]   ✓ Using default version (parent match)", completionId);
            } else {
                // Parse additionalVersions and find matching parent
                                Log(LogLevel::Verbose, "[KVStore V2 Lookup]   Default parent doesn't match, searching additional versions...", completionId);
                auto versions = ParseAdditionalVersions(additionalVersions);
                Log(LogLevel::Verbose, "[KVStore V2 Lookup]   Found " + std::to_string(versions.size()) + " additional versions", completionId);
                
                bool foundMatch = false;
                for (const auto& version : versions) {
                    if (version.parentHash == expectedParentHash) {
                        // Found matching version!
                        locationToRead = version.location;
                        blockHash = version.hash;
                        foundMatch = true;
                        Log(LogLevel::Verbose, "[KVStore V2 Lookup]   ✓ Found matching version: hash=" + std::to_string(version.hash) + 
                            ", parentHash=" + std::to_string(version.parentHash) + ", location=" + version.location, completionId);
                        break;
                    }
                }
                
                if (!foundMatch) {
                    Log(LogLevel::Error, "[KVStore V2 Lookup]   ✗ No version found with matching parent hash " + std::to_string(expectedParentHash), completionId);
                    Log(LogLevel::Error, "[KVStore V2 Lookup]   Breaking chain - cache miss", completionId);
                    break;
                }
            }
        } else {
            // Single version blob - validate parent
            if (blockNum > 0 && parentHash != expectedParentHash) {
                Log(LogLevel::Error, "[KVStore V2 Lookup]   ✗ Parent chain mismatch at block " + std::to_string(blockNum), completionId);
                break;
            }
        }
        
        // Add location to result
        result.locations.push_back(BlockLocation(blockHash, locationToRead));
        result.cachedBlocks++;
        result.lastHash = blockHash;
        expectedParentHash = blockHash;
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        Log(LogLevel::Information, "[KVStore V2 Lookup] ⏱️  Lookup took " + std::to_string(duration) + "ms, found " + std::to_string(result.cachedBlocks) + " blocks", completionId);
    
    return result;
}

// V2 API: ReadAsync - Read from location directly
std::future<std::pair<bool, PromptChunk>> AzureStorageKVStoreLibV2::ReadAsync(const std::string& location, const std::string& completionId) const {
    return std::async(std::launch::async, [this, location, completionId]() -> std::pair<bool, PromptChunk> {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        try {
            auto blobClient = blobContainerClient_->GetBlobClient(location);
            
            // Download includes metadata
            auto downloadResponse = blobClient.Download();
            
            // Extract metadata from download response
            std::string partitionKey;
            hash_t parentHash = 0;
            hash_t hash = 0;
            
            const auto& metadata = downloadResponse.Value.Details.Metadata;
            auto metaIt = metadata.find("partitionKey");
            if (metaIt != metadata.end()) {
                partitionKey = metaIt->second;
            }
            auto parentIt = metadata.find("parentHash");
            if (parentIt != metadata.end()) {
                parentHash = std::stoull(parentIt->second);
            }
            auto hashIt = metadata.find("hash");
            if (hashIt != metadata.end()) {
                hash = std::stoull(hashIt->second);
            }
            
            // Read blob content
            auto& bodyStream = downloadResponse.Value.BodyStream;
            std::vector<uint8_t> buffer;
            buffer.resize(static_cast<size_t>(downloadResponse.Value.BlobSize));
            bodyStream->ReadToCount(buffer.data(), buffer.size());
            
            PromptChunk chunk;
            chunk.partitionKey = partitionKey;
            chunk.parentHash = parentHash;
            chunk.hash = hash;
            chunk.bufferSize = buffer.size();
            chunk.buffer = std::move(buffer);
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
            Log(LogLevel::Information, "[KVStore V2 Read] ✓ Read successful from location: " + location + " (" + std::to_string(duration) + "ms)", completionId);
            
            return {true, chunk};
        } catch (const Azure::Storage::StorageException& ex) {
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
            Log(LogLevel::Error, "[KVStore V2 Read] ✗ Read failed from location: " + location + " - " + std::string(ex.what()) + " (" + std::to_string(duration) + "ms)", completionId);
            return {false, PromptChunk()};
        }
    });
}

// V2 API: WriteAsync - Multi-version blob support with conflict detection
std::future<void> AzureStorageKVStoreLibV2::WriteAsync(const PromptChunk& chunk) {
    return std::async(std::launch::async, [this, chunk]() {
        try {
            // Encode blob name from tokens
            std::string blobName = EncodeTokensToBlobName(chunk.tokens.cbegin(), chunk.tokens.cend());
            auto blockBlobClient = blobContainerClient_->GetBlobClient(blobName).AsBlockBlobClient();
            
            Log(LogLevel::Verbose, "[KVStore V2 Write] Writing chunk - Hash: " + std::to_string(chunk.hash) + 
                ", Parent: " + std::to_string(chunk.parentHash) + ", Blob: " + blobName, chunk.completionId);

            // Try to upload as first version using IfNoneMatch to detect server-side conflicts
            bool blobExists = false;
            std::string currentETag;
            std::unordered_map<std::string, std::string> metadata;
            
            try {
                // Use Upload API with IfNoneMatch=Any() for server-side conflict detection
                Azure::Core::IO::MemoryBodyStream contentStream(chunk.buffer.data(), chunk.bufferSize);
                Azure::Storage::Blobs::UploadBlockBlobOptions uploadOptions;
                uploadOptions.Metadata["hash"] = std::to_string(chunk.hash);
                uploadOptions.Metadata["parenthash"] = std::to_string(chunk.parentHash);
                uploadOptions.Metadata["location"] = blobName;
                uploadOptions.AccessConditions.IfNoneMatch = Azure::ETag::Any();  // Only succeed if blob doesn't exist

                auto uploadResponse = blockBlobClient.Upload(contentStream, uploadOptions);
                Log(LogLevel::Information, "[KVStore V2 Write]   ✓ First version uploaded successfully", chunk.completionId);
                return;
                
            } catch (const Azure::Storage::StorageException& ex) {
                // 409 Conflict means blob already exists - fall through to multi-version logic
                if (ex.StatusCode != Azure::Core::Http::HttpStatusCode::Conflict) {
                    throw;  // Re-throw if not a conflict
                }
                blobExists = true;
                Log(LogLevel::Verbose, "[KVStore V2 Write]   ⚠ Blob exists (conflict detected) - checking for version conflict", chunk.completionId);
            }
            
            // Blob exists - get metadata and check for version conflict
            auto propertiesResponse = blockBlobClient.GetProperties();
            currentETag = propertiesResponse.Value.ETag.ToString();
            // Convert CaseInsensitiveMap to unordered_map
            for (const auto& [key, value] : propertiesResponse.Value.Metadata) {
                metadata[key] = value;
                Log(LogLevel::Verbose, "[KVStore V2 Write]   → Read metadata key='" + key + "', value='" + value + "'", chunk.completionId);
            }
            
            // Log metadata immediately after reading
            Log(LogLevel::Verbose, "[KVStore V2 Write]   → Metadata after GetProperties: hash=" + 
                (metadata.find("hash") != metadata.end() ? metadata["hash"] : "NOT_FOUND") + 
                ", parenthash=" + 
                (metadata.find("parenthash") != metadata.end() ? metadata["parenthash"] : "NOT_FOUND"), chunk.completionId);

            // Check if same (hash, parentHash) already exists
            auto hashIt = metadata.find("hash");
            auto parentHashIt = metadata.find("parenthash");
            if (hashIt != metadata.end() && parentHashIt != metadata.end()) {
                if (hashIt->second == std::to_string(chunk.hash) && 
                    parentHashIt->second == std::to_string(chunk.parentHash)) {
                    Log(LogLevel::Information, "[KVStore V2 Write]   ✓ Identical version already exists - skipping", chunk.completionId);
                    return;
                }
            }

            // Check if already multi-version (has additionalVersions)
            std::vector<AdditionalVersion> existingVersions;
            auto versionsIt = metadata.find("additionalversions");
            if (versionsIt != metadata.end() && !versionsIt->second.empty()) {
                // Parse existing additionalVersions
                existingVersions = ParseAdditionalVersions(versionsIt->second);
                
                // Check if this version already exists in additional versions
                for (const auto& ver : existingVersions) {
                    if (ver.hash == chunk.hash && ver.parentHash == chunk.parentHash) {
                        Log(LogLevel::Information, "[KVStore V2 Write]   ✓ Version already exists in additionalVersions - skipping", chunk.completionId);
                        return;
                    }
                }
            }

            // New conflict - need to create GUID blob for additional version
            std::string guidLocation = GenerateGuid();
            Log(LogLevel::Verbose, "[KVStore V2 Write]   → Creating additional version blob: " + guidLocation, chunk.completionId);

            // Upload to GUID blob
            auto guidBlobClient = blobContainerClient_->GetBlobClient(guidLocation).AsBlockBlobClient();
            Azure::Core::IO::MemoryBodyStream guidContentStream(chunk.buffer.data(), chunk.bufferSize);
            Azure::Storage::Blobs::UploadBlockBlobOptions guidUploadOptions;
            guidUploadOptions.Metadata["hash"] = std::to_string(chunk.hash);
            guidUploadOptions.Metadata["parenthash"] = std::to_string(chunk.parentHash);
            guidUploadOptions.Metadata["location"] = guidLocation;
            
            auto guidUploadResponse = guidBlobClient.Upload(guidContentStream, guidUploadOptions);
            Log(LogLevel::Verbose, "[KVStore V2 Write]   ✓ GUID blob uploaded successfully", chunk.completionId);

            // Update metadata on default blob with retry (ETag-based optimistic concurrency)
            const int maxRetries = 5;
            for (int retry = 0; retry < maxRetries; ++retry) {
                try {
                    // Re-fetch properties if retry
                    if (retry > 0) {
                        auto retryPropertiesResponse = blockBlobClient.GetProperties();
                        currentETag = retryPropertiesResponse.Value.ETag.ToString();
                        // Convert CaseInsensitiveMap to unordered_map
                        metadata.clear();
                        for (const auto& [key, value] : retryPropertiesResponse.Value.Metadata) {
                            metadata[key] = value;
                        }
                        
                        // Re-parse versions in case they changed
                        auto versionsIt = metadata.find("additionalversions");
                        if (versionsIt != metadata.end()) {
                            existingVersions = ParseAdditionalVersions(versionsIt->second);
                        }
                    }

                    // Add new version
                    AdditionalVersion newVersion;
                    newVersion.hash = chunk.hash;
                    newVersion.parentHash = chunk.parentHash;
                    newVersion.location = guidLocation;
                    existingVersions.push_back(newVersion);

                    // FIFO eviction if capacity exceeded (~60 versions)
                    const size_t maxVersions = 60;
                    while (existingVersions.size() > maxVersions) {
                        auto oldestVersion = existingVersions.front();
                        Log(LogLevel::Verbose, "[KVStore V2 Write]   ⚠ Evicting oldest version: " + oldestVersion.location, chunk.completionId);
                        
                        // Delete oldest GUID blob
                        try {
                            auto oldBlobClient = blobContainerClient_->GetBlobClient(oldestVersion.location).AsBlockBlobClient();
                            oldBlobClient.Delete();
                            Log(LogLevel::Verbose, "[KVStore V2 Write]   ✓ Evicted blob: " + oldestVersion.location, chunk.completionId);
                        } catch (const Azure::Storage::StorageException& delEx) {
                            Log(LogLevel::Error, "[KVStore V2 Write]   ✗ Failed to evict blob: " + oldestVersion.location + 
                                " - " + delEx.what(), chunk.completionId);
                        }
                        
                        existingVersions.erase(existingVersions.begin());
                    }

                    // Serialize updated versions
                    std::string versionsJson = SerializeAdditionalVersions(existingVersions);

                    // Update metadata with ETag condition
                    Azure::Storage::Blobs::SetBlobMetadataOptions setMetadataOptions;
                    setMetadataOptions.AccessConditions.IfMatch = Azure::ETag(currentETag);
                    
                    metadata["additionalversions"] = versionsJson;

                    Log(LogLevel::Verbose, "[KVStore V2 Write]   → Setting additionalVersions count=" + 
                        std::to_string(existingVersions.size()), chunk.completionId);
                    
                    // Log metadata values before update
                    Log(LogLevel::Verbose, "[KVStore V2 Write]   → About to write metadata:", chunk.completionId);
                    Log(LogLevel::Verbose, "[KVStore V2 Write]     hash=" + metadata["hash"], chunk.completionId);
                    Log(LogLevel::Verbose, "[KVStore V2 Write]     parenthash=" + metadata["parenthash"], chunk.completionId);
                    Log(LogLevel::Verbose, "[KVStore V2 Write]     additionalVersions=" + versionsJson, chunk.completionId);

                    // Convert unordered_map to Azure::Storage::Metadata (CaseInsensitiveMap)
                    Azure::Storage::Metadata azureMetadata;
                    for (const auto& [key, value] : metadata) {
                        azureMetadata[key] = value;
                    }
                    auto setMetadataResponse = blockBlobClient.SetMetadata(azureMetadata, setMetadataOptions);
                    Log(LogLevel::Information, "[KVStore V2 Write]   ✓ Metadata updated successfully (retry " + 
                        std::to_string(retry + 1) + "/" + std::to_string(maxRetries) + ")", chunk.completionId);
                    return;
                    
                } catch (const Azure::Storage::StorageException& metadataEx) {
                    // ETag mismatch - concurrent update occurred
                    if (metadataEx.StatusCode == Azure::Core::Http::HttpStatusCode::PreconditionFailed) {
                        Log(LogLevel::Verbose, "[KVStore V2 Write]   ⚠ ETag mismatch on retry " + std::to_string(retry + 1) + 
                            " - retrying...", chunk.completionId);
                        if (retry == maxRetries - 1) {
                            Log(LogLevel::Error, "[KVStore V2 Write]   ✗ Max retries exceeded - giving up", chunk.completionId);
                            throw;
                        }
                        continue;
                    }
                    throw;
                }
            }
        } catch (const Azure::Storage::StorageException& ex) {
            Log(LogLevel::Error, "[KVStore V2 Write]   ✗ Azure StorageException: " + std::string(ex.what()), chunk.completionId);
            assert(false && "Azure StorageException during WriteAsync");
        }
    });
}

// Explicit instantiation for TokenIterator = std::vector<Token>::const_iterator
template LookupResult AzureStorageKVStoreLibV2::Lookup<std::vector<Token>::const_iterator>(
    const std::string&,
    const std::string&,
    std::vector<Token>::const_iterator,
    std::vector<Token>::const_iterator,
    const std::vector<hash_t>&
) const;



// Explicit template instantiation for std::vector<int64_t>::iterator
template LookupResult AzureStorageKVStoreLibV2::Lookup<std::vector<int64_t>::iterator>(
    const std::string&,
    const std::string&,
    std::vector<int64_t>::iterator,
    std::vector<int64_t>::iterator,
    const std::vector<hash_t>&
) const;
