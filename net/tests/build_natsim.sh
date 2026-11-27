#!/bin/bash
# Builds the NAT / relay simulator service used by the online pair tests: tools/net/build_natsim.sh <output file>
#   (NAT simulator, the test TURN server, and the few transport sources they use; plain clang++, no game headers)
set -e
cd "$(dirname "$0")/../.."
OUT=${1:?output file}
mkdir -p "$(dirname "$OUT")"
N=platform/sdk/net/online
exec clang++ ${M} -isystem /usr/include/x86_64-linux-gnu -O1 -std=gnu++11 -I. -Itools/net -fno-exceptions -fno-rtti \
  tools/net/natsim.cpp tools/net/turnsim.cpp tools/net/natsim_proxy.cpp $N/np_net.cpp $N/np_hash.cpp -o "$OUT" -lpthread
