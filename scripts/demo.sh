#!/usr/bin/env bash
# demo.sh — starts a 3-node cluster, writes some keys, kills a node,
# and shows gossip detecting the failure.

set -e
cd "$(dirname "$0")"

PHANTOM=./phantom
CLI=./bench/phantom-cli

if [ ! -f "$PHANTOM" ]; then
  echo "run 'make' first"
  exit 1
fi

cleanup() {
  echo ""
  echo "cleaning up..."
  kill "$NODE1_PID" "$NODE2_PID" "$NODE3_PID" 2>/dev/null || true
  rm -rf data/
}
trap cleanup EXIT

mkdir -p data/node-{1,2,3}

echo "==> starting node 1 (client=7600, gossip=7700)"
$PHANTOM 1 7600 7700 &
NODE1_PID=$!
sleep 0.3

echo "==> starting node 2 (client=7601, gossip=7701), seeding from node 1"
$PHANTOM 2 7601 7701 127.0.0.1:7700:1 &
NODE2_PID=$!
sleep 0.3

echo "==> starting node 3 (client=7602, gossip=7702), seeding from node 1"
$PHANTOM 3 7602 7702 127.0.0.1:7700:1 &
NODE3_PID=$!
sleep 1

echo ""
echo "==> writing 5 keys to node 1"
for i in 1 2 3 4 5; do
  echo "put key$i value$i" | $CLI 127.0.0.1 7600 2>/dev/null | grep -v "^connected\|^commands"
done

echo ""
echo "==> reading keys back from node 2"
for i in 1 2 3 4 5; do
  echo -n "  key$i: "
  echo "get key$i" | $CLI 127.0.0.1 7601 2>/dev/null | grep -v "^connected\|^commands"
done

echo ""
echo "==> killing node 3 (pid=$NODE3_PID)"
kill "$NODE3_PID" 2>/dev/null || true
NODE3_PID=""

echo "==> waiting for gossip to detect failure (~3 seconds)..."
sleep 3.5

echo ""
echo "==> gossip output from node 1 should show node 3 as suspect/dead"
echo "    (check stderr of node 1 in another terminal)"

echo ""
echo "==> cluster is still serving reads from node 1"
echo "get key1" | $CLI 127.0.0.1 7600 2>/dev/null | grep -v "^connected\|^commands"

echo ""
echo "demo complete. press Ctrl-C to stop."
wait "$NODE1_PID" "$NODE2_PID" 2>/dev/null || true
