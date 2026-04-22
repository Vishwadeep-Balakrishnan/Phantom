#ifndef PHANTOM_NET_H
#define PHANTOM_NET_H

#include <stdint.h>
#include <stddef.h>

/*
 * Binary protocol:
 *
 * Request:
 *   [op:1][key_len:2][val_len:4][key bytes][val bytes]
 *
 * Response:
 *   [status:1][val_len:4][val bytes]
 *
 * Opcodes:
 *   0x01  GET
 *   0x02  PUT
 *   0x03  DEL
 *
 * Status codes:
 *   0x00  OK
 *   0x01  NOT_FOUND
 *   0x02  ERROR
 */

#define PROTO_OP_GET   0x01
#define PROTO_OP_PUT   0x02
#define PROTO_OP_DEL   0x03

#define PROTO_OK        0x00
#define PROTO_NOT_FOUND 0x01
#define PROTO_ERROR     0x02

#define NET_MAX_EVENTS  256
#define NET_BACKLOG     512
#define NET_BUF_SIZE    (65536 + 512) /* max val + header */

struct phantom_node;

typedef struct {
    int    listen_fd;
    int    epoll_fd;
    int    running;
    struct phantom_node *node;
} net_server_t;

int  net_server_init(net_server_t *srv, uint16_t port, struct phantom_node *node);
int  net_server_run(net_server_t *srv);   /* blocks */
void net_server_stop(net_server_t *srv);

#endif
