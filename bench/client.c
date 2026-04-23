/*
 * client.c — minimal CLI for phantom.
 *
 * usage: ./phantom-cli <host> <port>
 *   > put mykey myvalue
 *   > get mykey
 *   > del mykey
 *   > quit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int send_all(int fd, const void *buf, size_t len) {
    size_t s = 0;
    while (s < len) {
        ssize_t n = send(fd, (const char *)buf + s, len - s, 0);
        if (n <= 0) return -1;
        s += n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len) {
    size_t g = 0;
    while (g < len) {
        ssize_t n = recv(fd, (char *)buf + g, len - g, 0);
        if (n <= 0) return -1;
        g += n;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(atoi(argv[2]));
    inet_pton(AF_INET, argv[1], &sa.sin_addr);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect");
        return 1;
    }

    printf("connected to %s:%s\n", argv[1], argv[2]);
    printf("commands: get <key>, put <key> <value>, del <key>, quit\n\n");

    char line[1024];
    uint8_t req[1024 + 64], resp[1024 + 8];

    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        char *tok = strtok(line, " \n");
        if (!tok) continue;
        if (strcmp(tok, "quit") == 0 || strcmp(tok, "exit") == 0) break;

        if (strcmp(tok, "get") == 0) {
            char *key = strtok(NULL, " \n");
            if (!key) { printf("usage: get <key>\n"); continue; }
            uint16_t klen = (uint16_t)strlen(key);
            req[0] = 0x01; req[1] = klen & 0xFF; req[2] = (klen>>8)&0xFF;
            req[3]=req[4]=req[5]=req[6]=0;
            memcpy(req+7, key, klen);
            send_all(fd, req, 7+klen);
            recv_all(fd, resp, 5);
            if (resp[0] == 0x01) { printf("(not found)\n"); continue; }
            uint32_t vlen = resp[1]|((uint32_t)resp[2]<<8)|((uint32_t)resp[3]<<16)|((uint32_t)resp[4]<<24);
            if (vlen) { recv_all(fd, resp+5, vlen); resp[5+vlen]='\0'; printf("%s\n", resp+5); }
            else printf("(empty value)\n");

        } else if (strcmp(tok, "put") == 0) {
            char *key = strtok(NULL, " \n");
            char *val = strtok(NULL, "\n");
            if (!key || !val) { printf("usage: put <key> <value>\n"); continue; }
            uint16_t klen = (uint16_t)strlen(key);
            uint32_t vlen = (uint32_t)strlen(val);
            req[0]=0x02; req[1]=klen&0xFF; req[2]=(klen>>8)&0xFF;
            req[3]=vlen&0xFF; req[4]=(vlen>>8)&0xFF; req[5]=(vlen>>16)&0xFF; req[6]=(vlen>>24)&0xFF;
            memcpy(req+7, key, klen); memcpy(req+7+klen, val, vlen);
            send_all(fd, req, 7+klen+vlen);
            recv_all(fd, resp, 5);
            printf(resp[0]==0 ? "OK\n" : "ERROR\n");

        } else if (strcmp(tok, "del") == 0) {
            char *key = strtok(NULL, " \n");
            if (!key) { printf("usage: del <key>\n"); continue; }
            uint16_t klen = (uint16_t)strlen(key);
            req[0]=0x03; req[1]=klen&0xFF; req[2]=(klen>>8)&0xFF;
            req[3]=req[4]=req[5]=req[6]=0;
            memcpy(req+7, key, klen);
            send_all(fd, req, 7+klen);
            recv_all(fd, resp, 5);
            printf(resp[0]==0 ? "OK\n" : "(not found)\n");

        } else {
            printf("unknown command: %s\n", tok);
        }
    }

    close(fd);
    return 0;
}
