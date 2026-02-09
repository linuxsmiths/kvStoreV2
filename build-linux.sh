#!/bin/bash

# Navigate to your kvStore directory
cd ~/kvStoreV2/

# Configure CMake
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=/home/azureuser/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release
