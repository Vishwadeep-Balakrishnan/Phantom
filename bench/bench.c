/*
 * bench.c — latency and throughput benchmark for phantom.
 *
 * Spawns N threads, each issuing M PUT/GET pairs to a single node.
 * Reports ops/sec and p50/p99/p999 latency.
 *
 * usage: ./bench <host> <port> <threads> <ops_per_thread>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_THREADS 128

static const char *g_host;
static int         g_port;
static int         g_ops;

static int connect_to_node(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(g_port);
    inet_pton(AF_INET, g_host, &sa.sin_addr);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int send_all(int fd, const void *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char *)buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, (char *)buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

typedef struct {
    int       thread_id;
    uint64_t *latencies;  /* ns per op */
    int       ops_done;
    int       errors;
} thread_arg_t;

static void *bench_thread(void *arg)
{
    thread_arg_t *ta = arg;
    int fd = connect_to_node();
    if (fd < 0) {
        fprintf(stderr, "thread %d: connect failed\n", ta->thread_id);
        return NULL;
    }

    char key[64], val[128];
    uint8_t req[512], resp[512];

    for (int i = 0; i < g_ops; i++) {
        int klen = snprintf(key, sizeof(key), "bench-t%d-k%d", ta->thread_id, i);
        int vlen = snprintf(val, sizeof(val), "value-%d-%d", ta->thread_id, i);

        /* PUT */
        req[0] = 0x02; /* PUT */
        req[1] = klen & 0xFF;
        req[2] = (klen >> 8) & 0xFF;
        req[3] = vlen & 0xFF;
        req[4] = (vlen >> 8) & 0xFF;
        req[5] = 0; req[6] = 0;
        memcpy(req + 7,        key, klen);
        memcpy(req + 7 + klen, val, vlen);

        uint64_t t0 = now_ns();
        if (send_all(fd, req, 7 + klen + vlen) < 0) { ta->errors++; continue; }
        if (recv_all(fd, resp, 5) < 0)               { ta->errors++; continue; }
        uint64_t t1 = now_ns();
        ta->latencies[i * 2] = t1 - t0;

        /* GET */
        req[0] = 0x01; /* GET */
        req[1] = klen & 0xFF;
        req[2] = (klen >> 8) & 0xFF;
        req[3] = req[4] = req[5] = req[6] = 0;
        memcpy(req + 7, key, klen);

        t0 = now_ns();
        if (send_all(fd, req, 7 + klen) < 0) { ta->errors++; continue; }
        if (recv_all(fd, resp, 5) < 0)        { ta->errors++; continue; }
        uint32_t resp_vlen = (uint32_t)(resp[1] | ((uint32_t)resp[2]<<8) |
                                        ((uint32_t)resp[3]<<16) | ((uint32_t)resp[4]<<24));
        if (resp_vlen > 0) {
            char tmp[512];
            if (recv_all(fd, tmp, resp_vlen) < 0) { ta->errors++; continue; }
        }
        t1 = now_ns();
        ta->latencies[i * 2 + 1] = t1 - t0;

        ta->ops_done++;
    }

    close(fd);
    return NULL;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <host> <port> <threads> <ops_per_thread>\n", argv[0]);
        return 1;
    }

    g_host    = argv[1];
    g_port    = atoi(argv[2]);
    int nthreads = atoi(argv[3]);
    g_ops        = atoi(argv[4]);

    if (nthreads > MAX_THREADS) nthreads = MAX_THREADS;

    pthread_t   threads[MAX_THREADS];
    thread_arg_t args[MAX_THREADS];

    /* Each thread issues g_ops PUT+GET pairs = 2*g_ops latency samples */
    uint64_t *all_lat = calloc((size_t)nthreads * g_ops * 2, sizeof(uint64_t));

    for (int i = 0; i < nthreads; i++) {
        args[i].thread_id = i;
        args[i].latencies = all_lat + (size_t)i * g_ops * 2;
        args[i].ops_done  = 0;
        args[i].errors    = 0;
    }

    uint64_t wall_start = now_ns();

    for (int i = 0; i < nthreads; i++)
        pthread_create(&threads[i], NULL, bench_thread, &args[i]);

    for (int i = 0; i < nthreads; i++)
        pthread_join(threads[i], NULL);

    uint64_t wall_end = now_ns();
    double   elapsed  = (double)(wall_end - wall_start) / 1e9;

    int total_ops = 0, total_errors = 0;
    for (int i = 0; i < nthreads; i++) {
        total_ops    += args[i].ops_done * 2; /* each op_done = 1 PUT + 1 GET */
        total_errors += args[i].errors;
    }

    /* Collect and sort latencies for percentiles */
    size_t nlat = (size_t)nthreads * g_ops * 2;
    qsort(all_lat, nlat, sizeof(uint64_t), cmp_u64);

    uint64_t p50  = all_lat[(size_t)(nlat * 0.50)];
    uint64_t p99  = all_lat[(size_t)(nlat * 0.99)];
    uint64_t p999 = all_lat[(size_t)(nlat * 0.999)];

    printf("\n--- phantom bench results ---\n");
    printf("threads:        %d\n",     nthreads);
    printf("ops/thread:     %d\n",     g_ops);
    printf("total ops:      %d\n",     total_ops);
    printf("errors:         %d\n",     total_errors);
    printf("elapsed:        %.3fs\n",  elapsed);
    printf("throughput:     %.0f ops/s\n", (double)total_ops / elapsed);
    printf("latency p50:    %.1f µs\n", (double)p50  / 1000.0);
    printf("latency p99:    %.1f µs\n", (double)p99  / 1000.0);
    printf("latency p99.9:  %.1f µs\n", (double)p999 / 1000.0);

    free(all_lat);
    return 0;
}
