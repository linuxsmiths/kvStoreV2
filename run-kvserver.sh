#!/bin/bash

#
# On my VM the nic is bound to numa node 1, also one numa node is sufficient
# to process the current load.
# If one numa node is not sufficient then run one server process on each numa
# node.
#
while :; do
	numactl --cpunodebind=1 --membind=1 ./build/KVService/KVStoreServer --port 8085 --log-level error --disable-metrics --disable-multi-nic &
	numactl --cpunodebind=1 --membind=1 ./build/KVService/KVStoreServer --port 8086 --log-level error --disable-metrics --disable-multi-nic &

	wait
done
