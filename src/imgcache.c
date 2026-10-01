/*
 * imgcache.c - the server's pictures kept on disc (see imgcache.h).
 * The directories are read with OS_GBPB 10 (names, lengths and date
 * stamps), made with OS_File 8; files are read and written with stdio.
 * Part of riscos-matinee. GPL v2 or later.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "imgcache.h"

#define OS_File 0x08
#define OS_GBPB 0x0C

#ifdef __riscos__
#define SEP "."
#else
#define SEP "/"                     /* the host test's files */
#endif

static char dir[256];
static long long max_size, size;
static int files;

static _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

static void make_dir(const char *path)
{
    _kernel_swi_regs r;
    r.r[0] = 8;
    r.r[1] = (intptr_t)path;
    r.r[4] = 0;
    swi(OS_File, &r);
}

/* FNV-1a, 64 bits, as 16 hex digits */
static void hash(const char *key, char *out)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        h = (h ^ *p) * 1099511628211ULL;
    snprintf(out, 17, "%08x%08x", (unsigned)(h >> 32), (unsigned)h);
}

static void path_of(const char *key, char *out, size_t size_out, int make)
{
    char h[17], sub[300];
    hash(key, h);
    snprintf(sub, sizeof(sub), "%s" SEP "%.2s", dir, h);
    if (make)
        make_dir(sub);
    snprintf(out, size_out, "%s" SEP "%.10s", sub, h + 2);    /* 10 characters: any FileCore format */
}

/* Every file: calls fn(path, length, stamp) for each; the stamps are
   RISC OS's 5-byte times (centiseconds since 1900), for the order */
typedef void (*each_fn)(const char *path, long len, long long stamp, void *ctx);

static void each(each_fn fn, void *ctx)
{
    static int buf[512];            /* 2KB of entries at a time */
    for (int d = 0; d < 256; d++) {
        char sub[300];
        int offset = 0;
        snprintf(sub, sizeof(sub), "%s" SEP "%02x", dir, d);
        do {
            _kernel_swi_regs r;
            const char *e;
            r.r[0] = 10;            /* read entries and their information */
            r.r[1] = (intptr_t)sub;
            r.r[2] = (intptr_t)buf;
            r.r[3] = 64;
            r.r[4] = offset;
            r.r[5] = sizeof(buf);
            r.r[6] = 0;             /* every name */
            if (swi(OS_GBPB, &r))
                break;              /* no such directory yet */
            e = (const char *)buf;
            for (int i = 0; i < r.r[3]; i++) {
                const int *w = (const int *)e;
                const char *name = e + 20;
                size_t n = strlen(name);
                if (w[4] == 1) {    /* a file */
                    char path[400];
                    long long stamp = ((long long)(w[0] & 0xFF) << 32) | (unsigned)w[1];
                    snprintf(path, sizeof(path), "%s" SEP "%s", sub, name);
                    fn(path, w[2], stamp, ctx);
                }
                e += (20 + n + 1 + 3) & ~3;
            }
            offset = r.r[4];
        } while (offset != -1);
    }
}

typedef struct { char path[64]; long len; long long stamp; } entry_t;
typedef struct { entry_t *v; int n, cap; } list_t;

static void count_one(const char *path, long len, long long stamp, void *ctx)
{
    list_t *l = ctx;
    (void)stamp;
    size += len;
    files++;
    if (!l)
        return;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        entry_t *v = realloc(l->v, cap * sizeof(*v));
        if (!v)
            return;
        l->v = v;
        l->cap = cap;
    }
    snprintf(l->v[l->n].path, sizeof(l->v[0].path), "%s", path + strlen(dir));    /* the part after dir */
    l->v[l->n].len = len;
    l->v[l->n].stamp = stamp;
    l->n++;
}

static int older(const void *a, const void *b)
{
    long long x = ((const entry_t *)a)->stamp, y = ((const entry_t *)b)->stamp;
    return x < y ? -1 : x > y;
}

void imgcache_init(const char *d, long long max_bytes)
{
    list_t l = { NULL, 0, 0 };
    snprintf(dir, sizeof(dir), "%s", d);
    max_size = max_bytes;
    make_dir(dir);
    size = 0;
    files = 0;
    each(count_one, &l);
    if (size > max_size && l.n) {   /* the oldest first, down to three quarters */
        qsort(l.v, l.n, sizeof(*l.v), older);
        for (int i = 0; i < l.n && size > max_size * 3 / 4; i++) {
            char path[400];
            snprintf(path, sizeof(path), "%s%s", dir, l.v[i].path);
            if (remove(path) == 0) {
                size -= l.v[i].len;
                files--;
            }
        }
    }
    free(l.v);
}

int imgcache_get(const char *key, char **data, size_t *len)
{
    char path[400];
    FILE *f;
    long n;
    *data = NULL;
    *len = 0;
    if (!*dir)
        return -1;
    path_of(key, path, sizeof(path), 0);
    if (!(f = fopen(path, "rb")))
        return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0 ||
        !(*data = malloc(n + 1)) || fread(*data, 1, n, f) != (size_t)n) {
        fclose(f);
        free(*data);
        *data = NULL;
        return -1;
    }
    fclose(f);
    (*data)[n] = 0;
    *len = n;
    return 0;
}

void imgcache_put(const char *key, const char *data, size_t len)
{
    char path[400];
    FILE *f;
    if (!*dir || !len || (max_size > 0 && size + (long long)len > max_size))
        return;                     /* full: kept until the next start trims it */
    path_of(key, path, sizeof(path), 1);
    if (!(f = fopen(path, "wb")))
        return;
    if (fwrite(data, 1, len, f) != len) {
        fclose(f);
        remove(path);
        return;
    }
    if (fclose(f) != 0) {
        remove(path);
        return;
    }
    size += len;
    files++;
}

long long imgcache_size(int *n)
{
    if (n)
        *n = files;
    return size;
}

static void remove_one(const char *path, long len, long long stamp, void *ctx)
{
    int *gone = ctx;
    (void)stamp;
    if (remove(path) == 0) {
        (*gone)++;
        size -= len;
        files--;
    }
}

int imgcache_clear(void)
{
    int gone = 0;
    if (!*dir)
        return 0;
    each(remove_one, &gone);
    size = 0;                       /* whatever's left is counted again next time */
    files = 0;
    return gone;
}
