#include "gossip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
 * Gossip protocol — simplified SWIM.
 *
 * Every GOSSIP_INTERVAL_MS milliseconds, the node:
 *   1. Increments its own heartbeat counter.
 *   2. Picks GOSSIP_FANOUT random live peers and sends them the full member list.
 *   3. Checks all members — if last_seen is stale, marks them SUSPECT then DEAD.
 *
 * Messages are JSON-ish plaintext over UDP. Small enough to fit in a single
 * MTU at realistic cluster sizes (<=64 nodes).
 *
 * Format: "GOSSIP <count>\n<id> <addr> <port> <state> <heartbeat>\n..."
 */

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

static void ms_sleep(long ms) __attribute__((unused));
static void ms_sleep(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Fisher-Yates to pick k random indices from [0, n) */
static void shuffle_indices(int *arr, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
    }
}

static int build_gossip_message(gossip_node_t *g, char *buf, size_t bufsz)
{
    int written = snprintf(buf, bufsz, "GOSSIP %d\n", g->member_count);
    for (int i = 0; i < g->member_count && written < (int)bufsz - 128; i++) {
        gossip_member_t *m = &g->members[i];
        written += snprintf(buf + written, bufsz - written,
                            "%d %s %d %d %llu\n",
                            m->id, m->addr, m->port,
                            (int)m->state,
                            (unsigned long long)m->heartbeat);
    }
    return written;
}

static void parse_gossip_message(gossip_node_t *g, const char *buf, size_t len)
{
    (void)len;

    int count = 0;
    const char *p = buf;
    if (sscanf(p, "GOSSIP %d\n", &count) != 1)
        return;
    p = strchr(p, '\n');
    if (!p) return;
    p++;

    uint64_t now = mono_ms();

    while (count-- > 0 && *p) {
        int      id, port, state;
        char     addr[64];
        unsigned long long hb;

        int consumed = 0;
        if (sscanf(p, "%d %63s %d %d %llu\n%n", &id, addr, &port, &state, &hb, &consumed) < 5)
            break;
        p += consumed;

        if (id == g->self_id)
            continue; /* ignore what others say about us — we know best */

        gossip_member_t *found = NULL;
        for (int i = 0; i < g->member_count; i++) {
            if (g->members[i].id == id) { found = &g->members[i]; break; }
        }

        if (!found) {
            if (g->member_count >= GOSSIP_MAX_NODES)
                continue;
            found = &g->members[g->member_count++];
            found->id = id;
            snprintf(found->addr, sizeof(found->addr), "%s", addr);
            found->port = (uint16_t)port;
            found->heartbeat  = 0;
            found->state      = NODE_DEAD;
            found->last_seen  = 0;
            fprintf(stderr, "[gossip] discovered new node %d at %s:%d\n", id, addr, port);
        }

        /* Only accept newer heartbeat info */
        if ((uint64_t)hb > found->heartbeat) {
            node_state_t old_state = found->state;
            found->heartbeat = (uint64_t)hb;
            found->last_seen = now;
            found->state     = NODE_ALIVE;

            if (found->state != old_state && g->on_change)
                g->on_change(g, found, old_state, NODE_ALIVE);
        }
    }
}

static void probe_members(gossip_node_t *g)
{
    uint64_t now = mono_ms();

    for (int i = 0; i < g->member_count; i++) {
        gossip_member_t *m = &g->members[i];
        if (m->id == g->self_id) continue;

        uint64_t age_rounds = (now - m->last_seen) / GOSSIP_INTERVAL_MS;

        if (m->state == NODE_ALIVE && age_rounds >= GOSSIP_DEAD_THRESH) {
            node_state_t old = m->state;
            m->state = NODE_SUSPECT;
            fprintf(stderr, "[gossip] node %d is suspect (no heartbeat for %llums)\n",
                    m->id, (unsigned long long)(now - m->last_seen));
            if (g->on_change) g->on_change(g, m, old, NODE_SUSPECT);

        } else if (m->state == NODE_SUSPECT && age_rounds >= GOSSIP_DEAD_THRESH * 2) {
            node_state_t old = m->state;
            m->state = NODE_DEAD;
            fprintf(stderr, "[gossip] node %d declared dead\n", m->id);
            if (g->on_change) g->on_change(g, m, old, NODE_DEAD);
        }
    }
}

static void fanout(gossip_node_t *g)
{
    char buf[8192];
    int  msglen = build_gossip_message(g, buf, sizeof(buf));

    /* Collect live peer indices */
    int peers[GOSSIP_MAX_NODES];
    int npeer = 0;
    for (int i = 0; i < g->member_count; i++) {
        if (g->members[i].id != g->self_id && g->members[i].state != NODE_DEAD)
            peers[npeer++] = i;
    }

    if (npeer == 0) return;

    shuffle_indices(peers, npeer);
    int targets = npeer < GOSSIP_FANOUT ? npeer : GOSSIP_FANOUT;

    for (int t = 0; t < targets; t++) {
        gossip_member_t *m = &g->members[peers[t]];
        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(m->port);
        inet_pton(AF_INET, m->addr, &sa.sin_addr);
        sendto(g->sock_fd, buf, msglen, 0,
               (struct sockaddr *)&sa, sizeof(sa));
    }
}

static void *gossip_thread(void *arg)
{
    gossip_node_t *g = arg;
    char recv_buf[16384];

    /* Non-blocking receive */
    struct timeval tv = { .tv_sec = 0, .tv_usec = GOSSIP_INTERVAL_MS * 1000 };
    setsockopt(g->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (g->running) {
        /* Bump our own heartbeat */
        pthread_mutex_lock(&g->lock);
        for (int i = 0; i < g->member_count; i++) {
            if (g->members[i].id == g->self_id) {
                g->members[i].heartbeat++;
                g->members[i].last_seen = mono_ms();
                break;
            }
        }

        probe_members(g);
        fanout(g);
        pthread_mutex_unlock(&g->lock);

        /* Drain incoming messages */
        ssize_t n;
        while ((n = recv(g->sock_fd, recv_buf, sizeof(recv_buf) - 1, 0)) > 0) {
            recv_buf[n] = '\0';
            pthread_mutex_lock(&g->lock);
            parse_gossip_message(g, recv_buf, (size_t)n);
            pthread_mutex_unlock(&g->lock);
        }
    }

    return NULL;
}

int gossip_init(gossip_node_t *g, int self_id, uint16_t port,
                on_member_change_fn cb)
{
    memset(g, 0, sizeof(*g));
    g->self_id   = self_id;
    g->port      = port;
    g->on_change = cb;
    g->running   = 0;

    pthread_mutex_init(&g->lock, NULL);

    /* Register ourselves */
    gossip_member_t *self = &g->members[g->member_count++];
    self->id        = self_id;
    self->port      = port;
    self->state     = NODE_ALIVE;
    self->heartbeat = 0;
    self->last_seen = mono_ms();
    strncpy(self->addr, "127.0.0.1", sizeof(self->addr) - 1);

    g->sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g->sock_fd < 0) return -errno;

    int opt = 1;
    setsockopt(g->sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa = {0};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (bind(g->sock_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(g->sock_fd);
        return -errno;
    }

    return 0;
}

int gossip_add_seed(gossip_node_t *g, const char *addr, uint16_t port, int id)
{
    pthread_mutex_lock(&g->lock);

    for (int i = 0; i < g->member_count; i++) {
        if (g->members[i].id == id) {
            pthread_mutex_unlock(&g->lock);
            return 0;
        }
    }

    if (g->member_count >= GOSSIP_MAX_NODES) {
        pthread_mutex_unlock(&g->lock);
        return -ENOSPC;
    }

    gossip_member_t *m = &g->members[g->member_count++];
    m->id        = id;
    m->port      = port;
    m->state     = NODE_ALIVE;
    m->heartbeat = 0;
    m->last_seen = mono_ms();
    strncpy(m->addr, addr, sizeof(m->addr) - 1);

    pthread_mutex_unlock(&g->lock);
    return 0;
}

int gossip_start(gossip_node_t *g)
{
    g->running = 1;
    return pthread_create(&g->thread, NULL, gossip_thread, g);
}

void gossip_stop(gossip_node_t *g)
{
    g->running = 0;
    pthread_join(g->thread, NULL);
    close(g->sock_fd);
}

int gossip_get_live_members(gossip_node_t *g, gossip_member_t *out, int max)
{
    pthread_mutex_lock(&g->lock);
    int n = 0;
    for (int i = 0; i < g->member_count && n < max; i++) {
        if (g->members[i].state == NODE_ALIVE && g->members[i].id != g->self_id)
            out[n++] = g->members[i];
    }
    pthread_mutex_unlock(&g->lock);
    return n;
}
