#ifndef PHANTOM_GOSSIP_H
#define PHANTOM_GOSSIP_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <netinet/in.h>

#define GOSSIP_MAX_NODES      64
#define GOSSIP_FANOUT         3
#define GOSSIP_INTERVAL_MS    500
#define GOSSIP_DEAD_THRESH    5   /* missed rounds before marking dead */
#define GOSSIP_PORT_BASE      7700

typedef enum {
    NODE_ALIVE   = 0,
    NODE_SUSPECT = 1,
    NODE_DEAD    = 2,
} node_state_t;

typedef struct {
    int          id;
    char         addr[64];
    uint16_t     port;
    node_state_t state;
    uint64_t     heartbeat;
    uint64_t     last_seen; /* monotonic ms */
} gossip_member_t;

typedef struct gossip_node gossip_node_t;

typedef void (*on_member_change_fn)(gossip_node_t *g, const gossip_member_t *m,
                                    node_state_t old_state, node_state_t new_state);

struct gossip_node {
    int               self_id;
    uint16_t          port;
    int               sock_fd;
    gossip_member_t   members[GOSSIP_MAX_NODES];
    int               member_count;
    pthread_mutex_t   lock;
    pthread_t         thread;
    int               running;
    on_member_change_fn on_change;
};

int  gossip_init(gossip_node_t *g, int self_id, uint16_t port,
                 on_member_change_fn cb);
int  gossip_add_seed(gossip_node_t *g, const char *addr, uint16_t port, int id);
int  gossip_start(gossip_node_t *g);
void gossip_stop(gossip_node_t *g);
int  gossip_get_live_members(gossip_node_t *g, gossip_member_t *out, int max);

#endif
