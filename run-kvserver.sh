#!/bin/bash

numactl --cpunodebind=0 --membind=0 ./build/KVService/KVStoreServer --port 8085 --log-level error --disable-metrics --disable-multi-nic &
numactl --cpunodebind=1 --membind=1 ./build/KVService/KVStoreServer --port 8086 --log-level error --disable-metrics --disable-multi-nic &

wait
