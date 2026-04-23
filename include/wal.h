#ifndef PHANTOM_WAL_H
#define PHANTOM_WAL_H

#include <stdint.h>
#include <stddef.h>
#include "store.h"

/*
 * Append-only write-ahead log.
 * Every mutation is written and fsynced before we ACK the client.
 * On startup we replay the log into the memtable.
 *
 * Record format (little-endian):
 *   [magic:4][op:1][ver:8][klen:2][vlen:4][key][val][crc32:4]
 */

#define WAL_MAGIC        0x504D5748u  /* WHMPH */
#define WAL_OP_PUT       0x01
#define WAL_OP_DEL       0x02

typedef struct {
    int      fd;
    char    *path;
} wal_t;

typedef int (*wal_apply_fn)(void *ctx, int op, const char *key, size_t klen,
                             const char *val, size_t vlen, uint64_t version);

int  wal_open(wal_t *w, const char *path);
void wal_close(wal_t *w);
int  wal_append_put(wal_t *w, const char *key, size_t klen,
                    const char *val, size_t vlen, uint64_t version);
int  wal_append_del(wal_t *w, const char *key, size_t klen);
int  wal_replay(const char *path, wal_apply_fn fn, void *ctx);

#endif
