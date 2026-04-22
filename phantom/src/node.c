#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static void on_member_change(gossip_node_t *g, const gossip_member_t *m,
                              node_state_t old_state, node_state_t new_state)
{
    /* The gossip layer calls us when a node's state changes.
     * We update the hash ring accordingly — add on ALIVE, remove on DEAD. */
    phantom_node_t *pn = (phantom_node_t *)((char *)g -
                          offsetof(phantom_node_t, gossip));

    if (old_state != NODE_ALIVE && new_state == NODE_ALIVE) {
        hash_ring_add_node(&pn->ring, m->id);
        fprintf(stderr, "[node] ring: added node %d\n", m->id);
    } else if (new_state == NODE_DEAD) {
        hash_ring_remove_node(&pn->ring, m->id);
        fprintf(stderr, "[node] ring: removed dead node %d\n", m->id);
    }
}

static int wal_apply(void *ctx, int op, const char *key, size_t klen,
                     const char *val, size_t vlen, uint64_t version)
{
    store_t *s = ctx;
    if (op == WAL_OP_PUT)
        return store_put(s, key, klen, val, vlen, version);
    if (op == WAL_OP_DEL)
        return store_del(s, key, klen);
    return 0;
}

int phantom_node_init(phantom_node_t *pn, int node_id,
                      uint16_t client_port, uint16_t gossip_port,
                      const char *data_dir)
{
    memset(pn, 0, sizeof(*pn));
    pn->node_id     = node_id;
    pn->client_port = client_port;
    pn->gossip_port = gossip_port;

    /* Create data directory if needed */
    mkdir(data_dir, 0755);

    int rc;

    rc = store_init(&pn->store);
    if (rc < 0) { fprintf(stderr, "store_init: %d\n", rc); return rc; }

    /* Replay WAL into the store before accepting connections */
    char wal_path[512];
    snprintf(wal_path, sizeof(wal_path), "%s/node-%d.wal", data_dir, node_id);

    int replayed = wal_replay(wal_path, wal_apply, &pn->store);
    if (replayed > 0)
        fprintf(stderr, "[node] replayed %d records from WAL\n", replayed);

    rc = wal_open(&pn->wal, wal_path);
    if (rc < 0) { fprintf(stderr, "wal_open: %d\n", rc); return rc; }

    hash_ring_init(&pn->ring);
    hash_ring_add_node(&pn->ring, node_id);

    rc = gossip_init(&pn->gossip, node_id, gossip_port, on_member_change);
    if (rc < 0) { fprintf(stderr, "gossip_init: %d\n", rc); return rc; }

    rc = mpmc_queue_init(&pn->work_queue, WORK_QUEUE_CAP);
    if (rc < 0) { fprintf(stderr, "queue_init: %d\n", rc); return rc; }

    rc = net_server_init(&pn->net, client_port, pn);
    if (rc < 0) { fprintf(stderr, "net_server_init: %d\n", rc); return rc; }

    fprintf(stderr, "[node %d] initialized (client=%d, gossip=%d)\n",
            node_id, client_port, gossip_port);
    return 0;
}

int phantom_node_start(phantom_node_t *pn)
{
    gossip_start(&pn->gossip);
    fprintf(stderr, "[node %d] gossip started\n", pn->node_id);
    return net_server_run(&pn->net); /* blocks */
}

void phantom_node_stop(phantom_node_t *pn)
{
    net_server_stop(&pn->net);
    gossip_stop(&pn->gossip);
    wal_close(&pn->wal);
    store_destroy(&pn->store);
    hash_ring_destroy(&pn->ring);
    mpmc_queue_destroy(&pn->work_queue);
}

int phantom_handle_get(phantom_node_t *pn,
                       const char *key, size_t klen,
                       char **val_out, size_t *vlen_out)
{
    return store_get(&pn->store, key, klen, val_out, vlen_out);
}

int phantom_handle_put(phantom_node_t *pn,
                       const char *key, size_t klen,
                       const char *val, size_t vlen)
{
    int rc = wal_append_put(&pn->wal, key, klen, val, vlen, 0);
    if (rc < 0) return rc;
    return store_put(&pn->store, key, klen, val, vlen, 0);
}

int phantom_handle_del(phantom_node_t *pn,
                       const char *key, size_t klen)
{
    int rc = wal_append_del(&pn->wal, key, klen);
    if (rc < 0) return rc;
    return store_del(&pn->store, key, klen);
}
