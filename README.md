# phantom

A distributed in-memory key-value store written in C. No external dependencies beyond POSIX and pthreads. Fits in a single terminal window.

I built this to understand how production systems like Cassandra and DynamoDB actually work at the protocol level, not just from reading about them.

## what it does

- Stores key-value pairs in memory with crash recovery via a write-ahead log
- Distributes keys across nodes using consistent hashing with virtual nodes (150 vnodes per physical node)
- Detects node failures using a gossip protocol based on SWIM
- Rebalances the hash ring automatically when nodes join or leave
- Handles multiple concurrent clients using a Linux epoll event loop

## design

There are six parts and they each do one thing:

**queue.c** — Lock-free MPMC queue using C11 atomics and compare-and-swap. Based on Dmitry Vyukov's sequence-number design. No mutexes, no kernel calls on the fast path.

**ring.c** — Consistent hash ring over MurmurHash3 token values. Binary search for O(log n) key lookup. Virtual nodes keep the load distribution balanced when nodes fail.

**store.c** — Open-addressing hash table with Robin Hood probing as the memtable. Robin Hood keeps probe distances short by displacing entries that are "closer to home" than the incoming one. Better worst-case than chained hashing because it's cache-friendly.

**wal.c** — Append-only write-ahead log with CRC32 per record. Every write is `fdatasync`'d before we send an ACK. On startup the log replays into the memtable, which is how you get crash recovery without a separate storage engine.

**gossip.c** — Simplified SWIM. Every 500ms each node increments its heartbeat counter, fans out to 3 random live peers, and checks if any members have gone quiet. After 5 missed rounds a node is marked suspect, after 10 it's declared dead and removed from the ring.

**net.c** — Edge-triggered epoll event loop with a non-blocking accept path. One thread, thousands of concurrent connections. Binary protocol keeps serialisation off the hot path.

## wire protocol

Requests and responses are binary, not text. Text protocols are fine for Redis at human latencies but binary cuts CPU time on the serialisation path.

Request:
```
[op:1][key_len:2][val_len:4][key bytes][val bytes]
```

Response:
```
[status:1][val_len:4][val bytes]
```

Opcodes: `0x01` GET, `0x02` PUT, `0x03` DEL

Status: `0x00` OK, `0x01` NOT_FOUND, `0x02` ERROR

## build

Requires gcc and Linux (the epoll event loop is Linux-specific). Tested on Ubuntu 24.04.

```sh
make
```

This builds three things: the `phantom` server, `bench/bench` for load testing, and `bench/phantom-cli` for interactive use.

## running a cluster

Start a single node:
```sh
./phantom 1 7600 7700
# args: <node_id> <client_port> <gossip_port>
```

Add more nodes and point them at node 1 as a seed:
```sh
./phantom 2 7601 7701 127.0.0.1:7700:1
./phantom 3 7602 7702 127.0.0.1:7700:1
```

The seed format is `addr:gossip_port:node_id`. Nodes exchange member lists over UDP and the ring updates automatically.

Or use the Makefile shortcut to get all three up at once:
```sh
make cluster
```

## interactive client

```sh
./bench/phantom-cli 127.0.0.1 7600
> put city london
OK
> get city
london
> del city
OK
> get city
(not found)
```

## benchmarks

```sh
# make sure the cluster is running first
make bench-run
```

This runs 8 threads each doing 10,000 PUT+GET pairs against node 1 and reports throughput and latency percentiles. On a mid-range laptop over loopback you should see something in the range of:

```
--- phantom bench results ---
threads:        8
ops/thread:     10000
total ops:      160000
errors:         0
elapsed:        1.4s
throughput:     ~115000 ops/s
latency p50:    ~60 µs
latency p99:    ~180 µs
latency p99.9:  ~400 µs
```

Numbers vary a lot depending on the machine and what else is running. The p99 is more interesting than throughput for latency-sensitive use cases.

## failure detection demo

```sh
./scripts/demo.sh
```

Starts a 3-node cluster, writes some keys, kills node 3, and waits for gossip to detect the failure. The ring update shows up in node 1's stderr output.

## data directory

Each node writes its WAL to `./data/node-<id>/node-<id>.wal`. The directory is created on startup. To wipe state and start fresh:

```sh
make clean
```

## project layout

```
phantom/
├── include/         headers for each module
│   ├── gossip.h
│   ├── net.h
│   ├── node.h
│   ├── queue.h
│   ├── ring.h
│   ├── store.h
│   └── wal.h
├── src/             implementations
│   ├── gossip.c
│   ├── main.c
│   ├── net.c
│   ├── node.c
│   ├── queue.c
│   ├── ring.c
│   ├── store.c
│   └── wal.c
├── bench/
│   ├── bench.c      load tester
│   └── client.c     interactive CLI
├── scripts/
│   └── demo.sh      cluster demo
└── Makefile
```


Each of these is a real problem with real solutions in the literature. The codebase is structured to make adding them possible without rewriting everything.

## license

MIT
