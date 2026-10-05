/*
 * imgcache.h - the server's pictures kept on disc: posters, backdrops and
 * cast photos, as the JPEGs the server sent, so the next time a list or a
 * details page is shown they come from the disc instead of the network.
 *
 * Kept in Choices (<Choices$Write>.Matinee.Cache, or Matinee$Cache), in 256
 * directories by the first two hex digits of a hash of the picture's key
 * (so no directory holds many files: FileCore's older formats hold 77).
 * Held to a size: the oldest go first. Clear image cache on the icon bar
 * menu empties it.
 * Part of riscos-matinee. GPL v2 or later.
 */
#ifndef MATINEE_IMGCACHE_H
#define MATINEE_IMGCACHE_H
#include <stddef.h>

/* Where it is (made if missing), and the most it may hold; it's trimmed
   to that now */
void imgcache_init(const char *dir, long long max_bytes);

/* The picture for key (any string: the server, its path, the size), if
   kept: 0 and *data (malloc'd, the caller frees) and *len; else -1 */
int imgcache_get(const char *key, char **data, size_t *len);
/* Keeps a picture */
void imgcache_put(const char *key, const char *data, size_t len);

/* 1 if data is a whole JPEG: it starts with SOI (FF D8) and ends with EOI
   (FF D9), allowing a few bytes of padding after it. A picture cut short
   on the way decodes as its top part, then black: not kept, and not used
   from the cache. */
int imgcache_jpeg_whole(const char *data, size_t len);

/* What it holds: bytes (files in *files if not NULL) */
long long imgcache_size(int *files);
/* Empties it; how many files went */
int imgcache_clear(void);

#endif
