#ifndef PHANTOM_NODE_H
#define PHANTOM_NODE_H

#include "store.h"
#include "wal.h"
#include "ring.h"
#include "gossip.h"
#include "net.h"
#include "queue.h"

#define PHANTOM_VERSION  "0.1.0"
#define WORK_QUEUE_CAP   4096

typedef struct phantom_node {
    int          node_id;
    uint16_t     client_port;   /* TCP port for client connections */
    uint16_t     gossip_port;   /* UDP port for gossip */

    store_t      store;
    wal_t        wal;
    hash_ring_t  ring;
    gossip_node_t gossip;
    net_server_t  net;
    mpmc_queue_t  work_queue;
} phantom_node_t;

int  phantom_node_init(phantom_node_t *pn, int node_id,
                       uint16_t client_port, uint16_t gossip_port,
                       const char *data_dir);
int  phantom_node_start(phantom_node_t *pn);
void phantom_node_stop(phantom_node_t *pn);

/* Called by net layer to handle a request */
int phantom_handle_get(phantom_node_t *pn,
                       const char *key, size_t klen,
                       char **val_out, size_t *vlen_out);
int phantom_handle_put(phantom_node_t *pn,
                       const char *key, size_t klen,
                       const char *val, size_t vlen);
int phantom_handle_del(phantom_node_t *pn,
                       const char *key, size_t klen);

#endif
