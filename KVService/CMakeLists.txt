cmake_minimum_required(VERSION 3.15)

# Option to use local Azure SDK build (for multi-NIC support)
# NOTE: Local SDK must be built first with custom multi-NIC features
# Set to ON to use local Azure SDK with multi-NIC support
if(WIN32)
	option(USE_LOCAL_AZURE_SDK "Use local Azure SDK build instead of vcpkg" ON)
else()
	option(USE_LOCAL_AZURE_SDK "Use local Azure SDK build instead of vcpkg" OFF)
endif()

# Configure local Azure SDK if enabled (BEFORE vcpkg toolchain)
if(USE_LOCAL_AZURE_SDK)
    set(CMAKE_PREFIX_PATH "C:/Users/kraman/azure-sdk-local-windows-mt")
    message(STATUS "Using local Azure SDK from: ${CMAKE_PREFIX_PATH}")
    message(STATUS "  Multi-NIC support and custom curl options ENABLED")
    
    add_compile_definitions(USE_LOCAL_AZURE_SDK=1)
    
    # Still need vcpkg for gRPC and protobuf
    if(NOT CMAKE_TOOLCHAIN_FILE)
        if(WIN32)
            set(CMAKE_TOOLCHAIN_FILE "C:/Users/kraman/source/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
        else()
            if(DEFINED ENV{VCPKG_ROOT})
                set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
            endif()
        endif()
    endif()
else()
    message(STATUS "Using official Azure SDK from vcpkg")
    message(STATUS "  Multi-NIC support DISABLED")
    add_compile_definitions(USE_LOCAL_AZURE_SDK=0)
    
    # Configure vcpkg BEFORE project() command
    if(NOT CMAKE_TOOLCHAIN_FILE)
        if(WIN32)
            set(CMAKE_TOOLCHAIN_FILE "C:/Users/kraman/source/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
        else()
            if(DEFINED ENV{VCPKG_ROOT})
                set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
            endif()
        endif()
    endif()
endif()

if(WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
else()
    set(VCPKG_TARGET_TRIPLET "x64-linux" CACHE STRING "")
endif()

set(BUILD_SHARED_LIBS OFF)

project(KVStoreGrpcService VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# Find Azure SDK - use local if enabled, otherwise vcpkg
if(USE_LOCAL_AZURE_SDK)
    find_package(azure-storage-blobs-cpp CONFIG REQUIRED PATHS "${CMAKE_PREFIX_PATH}" NO_DEFAULT_PATH)
    find_package(azure-identity-cpp CONFIG REQUIRED PATHS "${CMAKE_PREFIX_PATH}" NO_DEFAULT_PATH)
else()
    find_package(azure-storage-blobs-cpp CONFIG REQUIRED)
    find_package(azure-identity-cpp CONFIG REQUIRED)
endif()

# Proto file
set(PROTO_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/protos/kvstore.proto
)

# Generate C++ sources from proto files
set(PROTO_SRC_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_SRC_DIR})

set(PROTO_SRCS "${PROTO_SRC_DIR}/kvstore.pb.cc")
set(PROTO_HDRS "${PROTO_SRC_DIR}/kvstore.pb.h")
set(GRPC_SRCS "${PROTO_SRC_DIR}/kvstore.grpc.pb.cc")
set(GRPC_HDRS "${PROTO_SRC_DIR}/kvstore.grpc.pb.h")

add_custom_command(
    OUTPUT "${PROTO_SRCS}" "${PROTO_HDRS}" "${GRPC_SRCS}" "${GRPC_HDRS}"
    COMMAND protobuf::protoc
    ARGS --grpc_out="${PROTO_SRC_DIR}"
         --cpp_out="${PROTO_SRC_DIR}"
         -I "${CMAKE_CURRENT_SOURCE_DIR}/protos"
         --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
         "${CMAKE_CURRENT_SOURCE_DIR}/protos/kvstore.proto"
    DEPENDS "${PROTO_FILES}"
    COMMENT "Generating protobuf and gRPC C++ sources"
)

# KV Store Library V2 (using local files)
add_library(AzureStorageKVStoreLibV2
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AzureStorageKVStoreLibV2.cpp
)

target_include_directories(AzureStorageKVStoreLibV2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Force static runtime to match vcpkg libraries
target_compile_options(AzureStorageKVStoreLibV2 PRIVATE
    $<$<CONFIG:Release>:/MT>
    $<$<CONFIG:Debug>:/MTd>
)

# Add compile definition to exclude WinHTTP when using local SDK (curl-based)
if(USE_LOCAL_AZURE_SDK)
    target_compile_definitions(AzureStorageKVStoreLibV2 PRIVATE USE_CURL_TRANSPORT=1)
else()
    target_compile_definitions(AzureStorageKVStoreLibV2 PRIVATE USE_CURL_TRANSPORT=0)
endif()

# Link Azure SDK and dependencies
if(USE_LOCAL_AZURE_SDK)
    # Force local Azure SDK includes to come first
    target_include_directories(AzureStorageKVStoreLibV2 BEFORE PUBLIC
        "${CMAKE_PREFIX_PATH}/include"
    )
    
    # Include vcpkg curl headers for local SDK
    target_include_directories(AzureStorageKVStoreLibV2 PRIVATE
        "${CMAKE_SOURCE_DIR}/build/vcpkg_installed/x64-windows-static/include"
    )
    
    # Link directly to local Azure SDK static libraries
    target_link_libraries(AzureStorageKVStoreLibV2 PUBLIC
        # Local Azure SDK libs (static)
        "${CMAKE_PREFIX_PATH}/lib/azure-core.lib"
        "${CMAKE_PREFIX_PATH}/lib/azure-storage-blobs.lib"
        "${CMAKE_PREFIX_PATH}/lib/azure-storage-common.lib"
        "${CMAKE_PREFIX_PATH}/lib/azure-identity.lib"
        # vcpkg dependencies (libcurl, openssl, zlib) - link directly
        "${CMAKE_SOURCE_DIR}/build/vcpkg_installed/x64-windows-static/lib/libcurl.lib"
        "${CMAKE_SOURCE_DIR}/build/vcpkg_installed/x64-windows-static/lib/libssl.lib"
        "${CMAKE_SOURCE_DIR}/build/vcpkg_installed/x64-windows-static/lib/libcrypto.lib"
        "${CMAKE_SOURCE_DIR}/build/vcpkg_installed/x64-windows-static/lib/zlib.lib"
        # Windows system libraries needed by Azure SDK and curl
        ws2_32 crypt32 advapi32 bcrypt WebServices iphlpapi secur32 winhttp
    )
else()
    # Use vcpkg Azure SDK packages
    target_link_libraries(AzureStorageKVStoreLibV2 PUBLIC
        Azure::azure-storage-blobs
        Azure::azure-identity
        Azure::azure-core
    )

    if(WIN32)
        target_link_libraries(AzureStorageKVStoreLibV2 PUBLIC
            # Windows system libraries
            winhttp
            bcrypt
            crypt32
    )
    endif()
endif()

# KVStoreService library
add_library(KVStoreServiceLib
    src/KVStoreServiceImpl.cpp
    src/MetricsHelper.cpp
    src/InMemoryAccountResolver.cpp
    src/reactors/ReactorCommon.cpp
    src/reactors/LookupReactor.cpp
    src/reactors/ReadReactor.cpp
    src/reactors/WriteReactor.cpp
    src/reactors/StreamingReadReactor.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

target_include_directories(KVStoreServiceLib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/reactors
    ${PROTO_SRC_DIR}
    ${KVSTORE_LIB_PATH}/AzureStorageKVStoreLib/include
)

target_link_libraries(KVStoreServiceLib PUBLIC
    AzureStorageKVStoreLibV2
    gRPC::grpc++
    protobuf::libprotobuf
)

# Force static runtime to match vcpkg libraries
target_compile_options(KVStoreServiceLib PRIVATE
    $<$<CONFIG:Release>:/MT>
    $<$<CONFIG:Debug>:/MTd>
)

# Server executable
add_executable(KVStoreServer
    src/server.cpp
)

target_link_libraries(KVStoreServer PRIVATE
    KVStoreServiceLib
)

# Force static runtime to match vcpkg libraries
target_compile_options(KVStoreServer PRIVATE
    $<$<CONFIG:Release>:/MT>
    $<$<CONFIG:Debug>:/MTd>
)

# Install targets
install(TARGETS KVStoreServer
    RUNTIME DESTINATION bin
)

# Copy proto files to install directory
install(FILES ${PROTO_FILES}
    DESTINATION include/protos
)
