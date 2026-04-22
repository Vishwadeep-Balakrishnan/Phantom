#include "wal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

/*
 * CRC32 — polynomial 0xEDB88320 (IEEE 802.3)
 * We compute it inline; no need to pull in zlib.
 */
static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    static const uint32_t table[256] = {
        #define F(n) \
            (((n) & 1 ? 0xEDB88320u : 0) ^ \
             (((n) >> 1) & 1 ? 0xEDB88320u >> 1 : 0) ^ \
             (((n) >> 2) & 1 ? 0xEDB88320u >> 2 : 0) ^ \
             (((n) >> 3) & 1 ? 0xEDB88320u >> 3 : 0) ^ \
             (((n) >> 4) & 1 ? 0xEDB88320u >> 4 : 0) ^ \
             (((n) >> 5) & 1 ? 0xEDB88320u >> 5 : 0) ^ \
             (((n) >> 6) & 1 ? 0xEDB88320u >> 6 : 0) ^ \
             (((n) >> 7) & 1 ? 0xEDB88320u >> 7 : 0))
        0 /* placeholder — we build the table at runtime below */
    };
    (void)table;

    /* Build at first call */
    static uint32_t t[256];
    static int init = 0;
    if (!init) {
        for (int i = 0; i < 256; i++) {
            uint32_t c = (uint32_t)i;
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
            t[i] = c;
        }
        init = 1;
    }

    crc ^= 0xFFFFFFFFu;
    const uint8_t *p = data;
    while (len--)
        crc = (crc >> 8) ^ t[(crc ^ *p++) & 0xFF];
    return crc ^ 0xFFFFFFFFu;
}

static void write_le16(uint8_t *buf, uint16_t v)
{
    buf[0] = v & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
}

static void write_le32(uint8_t *buf, uint32_t v)
{
    buf[0] = v & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
    buf[2] = (v >> 16) & 0xFF;
    buf[3] = (v >> 24) & 0xFF;
}

static void write_le64(uint8_t *buf, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        buf[i] = (v >> (8 * i)) & 0xFF;
}

static uint16_t read_le16(const uint8_t *b) { return (uint16_t)(b[0] | ((uint16_t)b[1] << 8)); }
static uint32_t read_le32(const uint8_t *b) { return b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24); }
static uint64_t read_le64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)b[i] << (8*i));
    return v;
}

int wal_open(wal_t *w, const char *path)
{
    w->path = strdup(path);
    w->fd   = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (w->fd < 0) return -errno;
    return 0;
}

void wal_close(wal_t *w)
{
    if (w->fd >= 0) close(w->fd);
    free(w->path);
    w->fd   = -1;
    w->path = NULL;
}

static int wal_write_record(wal_t *w, uint8_t op,
                            const char *key, size_t klen,
                            const char *val, size_t vlen,
                            uint64_t version)
{
    /* header: magic(4) + op(1) + ver(8) + klen(2) + vlen(4) = 19 bytes */
    uint8_t hdr[19];
    write_le32(hdr,      WAL_MAGIC);
    hdr[4] = op;
    write_le64(hdr + 5,  version);
    write_le16(hdr + 13, (uint16_t)klen);
    write_le32(hdr + 15, (uint32_t)vlen);

    uint32_t crc = crc32_update(0, hdr, sizeof(hdr));
    crc = crc32_update(crc, key, klen);
    if (vlen) crc = crc32_update(crc, val, vlen);

    uint8_t crc_buf[4];
    write_le32(crc_buf, crc);

    struct iovec iov[4];
    int iovcnt = 0;
    iov[iovcnt].iov_base = hdr;  iov[iovcnt].iov_len = sizeof(hdr); iovcnt++;
    iov[iovcnt].iov_base = (void*)key; iov[iovcnt].iov_len = klen;  iovcnt++;
    if (vlen) {
        iov[iovcnt].iov_base = (void*)val; iov[iovcnt].iov_len = vlen; iovcnt++;
    }
    iov[iovcnt].iov_base = crc_buf; iov[iovcnt].iov_len = 4; iovcnt++;

    /* writev so the record is atomic on the kernel side */
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) total += iov[i].iov_len;

    uint8_t *flat = malloc(total);
    if (!flat) return -ENOMEM;
    size_t off = 0;
    for (int i = 0; i < iovcnt; i++) {
        memcpy(flat + off, iov[i].iov_base, iov[i].iov_len);
        off += iov[i].iov_len;
    }

    ssize_t n = write(w->fd, flat, total);
    free(flat);
    if (n != total) return -EIO;

    /* data must hit disk before we ACK */
    if (fdatasync(w->fd) < 0) return -errno;

    return 0;
}

int wal_append_put(wal_t *w, const char *key, size_t klen,
                   const char *val, size_t vlen, uint64_t version)
{
    return wal_write_record(w, WAL_OP_PUT, key, klen, val, vlen, version);
}

int wal_append_del(wal_t *w, const char *key, size_t klen)
{
    return wal_write_record(w, WAL_OP_DEL, key, klen, NULL, 0, 0);
}

int wal_replay(const char *path, wal_apply_fn fn, void *ctx)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return 0; /* no log yet, that's fine */
        return -errno;
    }

    int      replayed = 0;
    uint8_t  hdr[19];

    while (1) {
        ssize_t n = read(fd, hdr, sizeof(hdr));
        if (n == 0) break;  /* clean EOF */
        if (n != (ssize_t)sizeof(hdr)) goto truncated;

        if (read_le32(hdr) != WAL_MAGIC) goto corrupted;

        uint8_t  op      = hdr[4];
        uint64_t version = read_le64(hdr + 5);
        uint16_t klen    = read_le16(hdr + 13);
        uint32_t vlen    = read_le32(hdr + 15);

        char *key = malloc(klen + 1);
        char *val = vlen ? malloc(vlen + 1) : NULL;
        if (!key || (vlen && !val)) { free(key); free(val); close(fd); return -ENOMEM; }

        if (read(fd, key, klen) != (ssize_t)klen) { free(key); free(val); goto truncated; }
        key[klen] = '\0';
        if (vlen && read(fd, val, vlen) != (ssize_t)vlen) { free(key); free(val); goto truncated; }
        if (val) val[vlen] = '\0';

        uint8_t crc_buf[4];
        if (read(fd, crc_buf, 4) != 4) { free(key); free(val); goto truncated; }
        uint32_t stored_crc = read_le32(crc_buf);

        uint32_t crc = crc32_update(0, hdr, sizeof(hdr));
        crc = crc32_update(crc, key, klen);
        if (vlen) crc = crc32_update(crc, val, vlen);

        if (crc != stored_crc) {
            fprintf(stderr, "wal: crc mismatch at record %d — skipping\n", replayed);
            free(key); free(val);
            continue;
        }

        fn(ctx, op, key, klen, val, vlen, version);
        free(key);
        free(val);
        replayed++;
    }

    close(fd);
    return replayed;

truncated:
    fprintf(stderr, "wal: truncated record — log may be partially written, ignoring tail\n");
    close(fd);
    return replayed;

corrupted:
    fprintf(stderr, "wal: bad magic — log corrupted\n");
    close(fd);
    return -EIO;
}
