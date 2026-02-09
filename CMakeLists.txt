cmake_minimum_required(VERSION 3.15)
project(KVStoreV2 VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repository structure:
# - KVService: Windows gRPC server
# - KVClient: Linux gRPC client library with AzureStorageKVStoreLibV2 interface
# - KVPlayground: Linux test application using KVClient

# Build instructions:
# Windows (KVService): Use KVService/build_with_local_sdk.ps1
# Linux (KVClient + KVPlayground): Use this CMakeLists.txt

if(UNIX)
    message(STATUS "Building for Linux: KVClient + KVPlayground")
    add_subdirectory(KVClient)
    add_subdirectory(KVPlayground)
    add_subdirectory(KVService)
elseif(WIN32)
    message(WARNING "For Windows builds, use KVService/build_with_local_sdk.ps1")
    message(WARNING "This root CMakeLists.txt is for Linux builds only")
endif()
