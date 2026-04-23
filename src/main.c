#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static phantom_node_t g_node;
static volatile int   g_stop = 0;

static void sig_handler(int sig)
{
    (void)sig;
    g_stop = 1;
    net_server_stop(&g_node.net);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "phantom %s\n\n"
        "usage: %s <node_id> <client_port> <gossip_port> [seed_addr:gossip_port:node_id ...]\n\n"
        "examples:\n"
        "  start first node:\n"
        "    %s 1 7600 7700\n\n"
        "  start second node, seeding from first:\n"
        "    %s 2 7601 7701 127.0.0.1:7700:1\n\n"
        "  start third node, seeding from first:\n"
        "    %s 3 7602 7702 127.0.0.1:7700:1\n",
        PHANTOM_VERSION, prog, prog, prog, prog);
    exit(1);
}

int main(int argc, char *argv[])
{
    if (argc < 4)
        usage(argv[0]);

    int      node_id     = atoi(argv[1]);
    uint16_t client_port = (uint16_t)atoi(argv[2]);
    uint16_t gossip_port = (uint16_t)atoi(argv[3]);

    if (node_id <= 0 || client_port == 0 || gossip_port == 0)
        usage(argv[0]);

    char data_dir[256];
    snprintf(data_dir, sizeof(data_dir), "./data/node-%d", node_id);

    int rc = phantom_node_init(&g_node, node_id, client_port, gossip_port, data_dir);
    if (rc < 0) {
        fprintf(stderr, "failed to initialize node: %d\n", rc);
        return 1;
    }

    /* Parse seed nodes: addr:gossip_port:node_id */
    for (int i = 4; i < argc; i++) {
        char   seed_buf[256];
        strncpy(seed_buf, argv[i], sizeof(seed_buf) - 1);

        char *addr    = strtok(seed_buf, ":");
        char *port_s  = strtok(NULL, ":");
        char *id_s    = strtok(NULL, ":");

        if (!addr || !port_s || !id_s) {
            fprintf(stderr, "bad seed format: %s (expected addr:port:id)\n", argv[i]);
            continue;
        }

        int      seed_id   = atoi(id_s);
        uint16_t seed_port = (uint16_t)atoi(port_s);
        gossip_add_seed(&g_node.gossip, addr, seed_port, seed_id);
        hash_ring_add_node(&g_node.ring, seed_id);
        fprintf(stderr, "[main] added seed: node %d at %s:%d\n", seed_id, addr, seed_port);
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "[node %d] ready. client port %d, gossip port %d\n",
            node_id, client_port, gossip_port);

    phantom_node_start(&g_node);  /* blocks until stopped */
    phantom_node_stop(&g_node);

    fprintf(stderr, "[node %d] shutdown complete\n", node_id);
    return 0;
}
