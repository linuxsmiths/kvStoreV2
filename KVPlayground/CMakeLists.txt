cmake_minimum_required(VERSION 3.15)
project(KVPlayground VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Note: KVClient is added by root CMakeLists.txt, no need to add it again

# Find nlohmann_json
find_package(nlohmann_json CONFIG REQUIRED)

# Executable
add_executable(KVPlayground
    src/main.cpp
)

# Link against KVClient (which provides the gRPC implementation)
target_link_libraries(KVPlayground PRIVATE
    kvstore_grpc_client
    nlohmann_json::nlohmann_json
)

# Include directories
target_include_directories(KVPlayground PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../KVClient/include
)

# Compiler options for Linux
if(UNIX)
    target_compile_options(KVPlayground PRIVATE -Wall -Wextra -pthread)
    target_link_libraries(KVPlayground PRIVATE pthread)
endif()

set_target_properties(KVPlayground PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

find_package(Python3 REQUIRED)

# Copy chunk.bin and conversation_tokens.json to output directory after build
add_custom_command(TARGET KVPlayground POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/precompute_tokens.py
            ${CMAKE_CURRENT_LIST_DIR}/conversation_template.txt
            ${CMAKE_CURRENT_LIST_DIR}/conversation_tokens.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/chunk.bin
        $<TARGET_FILE_DIR:KVPlayground>/chunk.bin
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/conversation_tokens.json
        $<TARGET_FILE_DIR:KVPlayground>/conversation_tokens.json
    COMMENT "Copying chunk.bin and conversation_tokens.json to output directory"
)
