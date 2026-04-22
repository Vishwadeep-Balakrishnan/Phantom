#include "net.h"
#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

typedef struct {
    int      fd;
    uint8_t  buf[NET_BUF_SIZE];
    size_t   buf_len;
} conn_t;

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void handle_request(net_server_t *srv, conn_t *conn)
{
    if (conn->buf_len < 7)
        return; /* need at least op + key_len + val_len */

    uint8_t  op      = conn->buf[0];
    uint16_t key_len = (uint16_t)(conn->buf[1] | ((uint16_t)conn->buf[2] << 8));
    uint32_t val_len = (uint32_t)(conn->buf[3] | ((uint32_t)conn->buf[4] << 8) |
                                  ((uint32_t)conn->buf[5] << 16) | ((uint32_t)conn->buf[6] << 24));

    size_t expected = 7 + key_len + val_len;
    if (conn->buf_len < expected)
        return; /* not enough data yet */

    const char *key = (const char *)(conn->buf + 7);
    const char *val = (const char *)(conn->buf + 7 + key_len);

    phantom_node_t *pn = srv->node;

    uint8_t resp[NET_BUF_SIZE];
    size_t  resp_len = 0;

    if (op == PROTO_OP_GET) {
        char  *out_val  = NULL;
        size_t out_vlen = 0;
        int rc = phantom_handle_get(pn, key, key_len, &out_val, &out_vlen);
        if (rc == 0) {
            resp[0] = PROTO_OK;
            resp[1] = out_vlen & 0xFF;
            resp[2] = (out_vlen >> 8) & 0xFF;
            resp[3] = (out_vlen >> 16) & 0xFF;
            resp[4] = (out_vlen >> 24) & 0xFF;
            memcpy(resp + 5, out_val, out_vlen);
            resp_len = 5 + out_vlen;
            free(out_val);
        } else {
            resp[0] = PROTO_NOT_FOUND;
            resp[1] = resp[2] = resp[3] = resp[4] = 0;
            resp_len = 5;
        }

    } else if (op == PROTO_OP_PUT) {
        int rc = phantom_handle_put(pn, key, key_len, val, val_len);
        resp[0] = (rc == 0) ? PROTO_OK : PROTO_ERROR;
        resp[1] = resp[2] = resp[3] = resp[4] = 0;
        resp_len = 5;

    } else if (op == PROTO_OP_DEL) {
        int rc = phantom_handle_del(pn, key, key_len);
        resp[0] = (rc == 0) ? PROTO_OK : PROTO_NOT_FOUND;
        resp[1] = resp[2] = resp[3] = resp[4] = 0;
        resp_len = 5;

    } else {
        resp[0] = PROTO_ERROR;
        resp[1] = resp[2] = resp[3] = resp[4] = 0;
        resp_len = 5;
    }

    /* Shift processed bytes out of the buffer */
    memmove(conn->buf, conn->buf + expected, conn->buf_len - expected);
    conn->buf_len -= expected;

    send(conn->fd, resp, resp_len, MSG_NOSIGNAL);
}

int net_server_init(net_server_t *srv, uint16_t port, phantom_node_t *node)
{
    srv->node    = node;
    srv->running = 0;

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) return -errno;

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(srv->listen_fd, IPPROTO_TCP, TCP_NODELAY,  &opt, sizeof(opt));

    struct sockaddr_in sa = {0};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(srv->listen_fd);
        return -errno;
    }
    if (listen(srv->listen_fd, NET_BACKLOG) < 0) {
        close(srv->listen_fd);
        return -errno;
    }
    set_nonblocking(srv->listen_fd);

    srv->epoll_fd = epoll_create1(0);
    if (srv->epoll_fd < 0) {
        close(srv->listen_fd);
        return -errno;
    }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

    return 0;
}

int net_server_run(net_server_t *srv)
{
    struct epoll_event events[NET_MAX_EVENTS];
    srv->running = 1;

    fprintf(stderr, "[net] event loop started\n");

    while (srv->running) {
        int nev = epoll_wait(srv->epoll_fd, events, NET_MAX_EVENTS, 100);

        for (int i = 0; i < nev; i++) {
            int fd = events[i].data.fd;

            if (fd == srv->listen_fd) {
                /* Accept new connections */
                struct sockaddr_in ca;
                socklen_t cal = sizeof(ca);
                int cfd = accept(srv->listen_fd, (struct sockaddr *)&ca, &cal);
                if (cfd < 0) continue;

                set_nonblocking(cfd);
                int opt = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

                conn_t *conn = calloc(1, sizeof(conn_t));
                if (!conn) { close(cfd); continue; }
                conn->fd = cfd;

                struct epoll_event cev = {
                    .events   = EPOLLIN | EPOLLET,
                    .data.ptr = conn,
                };
                epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, cfd, &cev);

            } else {
                conn_t *conn = events[i].data.ptr;

                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    close(conn->fd);
                    free(conn);
                    continue;
                }

                /* Read available data */
                ssize_t n;
                while ((n = recv(conn->fd, conn->buf + conn->buf_len,
                                 sizeof(conn->buf) - conn->buf_len, 0)) > 0) {
                    conn->buf_len += (size_t)n;
                    handle_request(srv, conn);
                }

                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    close(conn->fd);
                    free(conn);
                }
            }
        }
    }

    return 0;
}

void net_server_stop(net_server_t *srv)
{
    srv->running = 0;
    close(srv->epoll_fd);
    close(srv->listen_fd);
}
