/*
 * pictures.c - the posters, photos and backdrops: the sprites made from them (kept
 * in memory, and on disc by imgcache.c), and fetching them without the
 * desktop waiting (bgreq.c).
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "ui_int.h"

/* ---- posters ------------------------------------------------------------------ */

static void pf_cancel(void);

void cache_free_all(void)
{
    pf_cancel();                    /* (a picture on its way is the old server's, or the old size) */
    while (S.cache) {
        poster_t *p = S.cache;
        S.cache = p->next;
        free(p->area);
        free(p);
    }
    S.cache_bytes = 0;
    for (int i = 0; S.disp && i < S.list.n; i++)
        S.disp[i].poster = NULL;
    S.det_art = S.det_poster = NULL;
    S.show.art = S.show.poster = NULL;
    S.show.fw = 0;
    S.home.art = NULL;
    S.home.fw = 0;
    for (int i = 0; i < S.ncast; i++)
        S.cast[i].photo = NULL;
    for (int k = 0; k < REL_ROWS; k++)
        for (int i = 0; i < 16; i++)
            S.relc[k][i].pic = NULL;
    S.cast_wanted = S.ncast > 0 || S.nrel[0] || S.nrel[1] || S.nrel[2];
}

/* Frees posters the list shown doesn't use, once the cache is big */
void cache_trim(void)
{
    poster_t **pp = &S.cache;
    if (S.cache_bytes <= POSTER_CACHE_MAX)
        return;
    while (*pp) {
        poster_t *p = *pp;
        int used = 0;
        for (int i = 0; S.disp && i < S.list.n && !used; i++)
            used = S.disp[i].poster == p;
        for (int i = 0; i < S.ncast && !used; i++)
            used = S.cast[i].photo == p;
        for (int k = 0; k < REL_ROWS && !used; k++)
            for (int i = 0; i < S.nrel[k] && !used; i++)
                used = S.relc[k][i].pic == p;
        if (!used && p != S.det_art && p != S.det_poster && p != S.show.art && p != S.show.poster && p != S.home.art) {
            *pp = p->next;
            S.cache_bytes -= p->bytes;
            free(p->area);
            free(p);
        } else {
            pp = &p->next;
        }
    }
}

static unsigned str_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

/* (A list of 2000 asks this for each item against a few hundred kept) */
poster_t *cache_find(const char *thumb)
{
    unsigned h = str_hash(thumb);
    for (poster_t *p = S.cache; p; p = p->next)
        if (p->hash == h && !strcmp(p->thumb, thumb))
            return p;
    return NULL;
}

/* A sprite area with one 32bpp sprite "p", w x h pixels at the screen's
   resolution, filled with the window's grey. Made by hand (the header's
   layout is fixed), so no SWI is needed. round > 0: a 1bpp mask that
   rounds its corners (round pixels). */
static int *sprite_make(int w, int h, int round, size_t *bytes)
{
    size_t image = (size_t)w * h * 4, words = (size_t)(w + 31) / 32;
    size_t mask = round > 0 ? words * 4 * h : 0, total = 16 + 44 + image + mask;
    int *a = malloc(total);
    int *s;
    unsigned *px;
    if (!a)
        return NULL;
    a[0] = (int)total;              /* area size */
    a[1] = 1;                       /* one sprite */
    a[2] = 16;                      /* the first at +16 */
    a[3] = (int)total;              /* free space after it */
    s = a + 4;
    s[0] = (int)(44 + image + mask);        /* to the next sprite */
    memset(&s[1], 0, 12);
    ((char *)&s[1])[0] = 'p';
    s[4] = w - 1;                   /* width in words - 1 (32bpp: one word a pixel) */
    s[5] = h - 1;
    s[6] = 0;
    s[7] = 31;
    s[8] = 44;                      /* image */
    s[9] = (int)(44 + (mask ? image : 0));  /* the mask (or none: the image again) */
    /* new-format mode word: 32bpp (type 6), the screen's dpi */
    s[10] = (int)(1u | ((unsigned)(180 >> S.xeig) << 1) | ((unsigned)(180 >> S.yeig) << 14) | (6u << 27));
    px = (unsigned *)(s + 11);
    for (size_t i = 0; i < (size_t)w * h; i++)
        px[i] = 0x001F1A18;         /* C_BG as 0xBBGGRR */
    if (mask)                       /* all solid; sprite_round() cuts the corners */
        memset((char *)s + 44 + image, 0xFF, mask);
    *bytes = total;
    return a;
}

/* The JPEG drawn into the sprite, in the middle: as big as fits, or
   (cover) filling it, cut at the edges. 0 = ok. */
static int jpeg_into(int *area, int w, int h, const char *jpeg, size_t len, int cover)
{
    _kernel_swi_regs r, out;
    int jw, jh, tw, th, scale[4];
    _kernel_oserror *e;

    r.r[0] = 1;                     /* the dimensions */
    r.r[1] = (intptr_t)jpeg;
    r.r[2] = (int)len;
    if (swi(JPEG_Info, &r))
        return -1;
    jw = r.r[2];
    jh = r.r[3];
    if (jw <= 0 || jh <= 0)
        return -1;
    if (((int64_t)w * jh <= (int64_t)h * jw) != !!cover) {   /* width decides */
        tw = w;
        th = (int)((int64_t)jh * w / jw);
    } else {
        th = h;
        tw = (int)((int64_t)jw * h / jh);
    }
    scale[0] = tw; scale[1] = th; scale[2] = jw; scale[3] = jh;

    r.r[0] = 60 + 512;              /* output to the sprite (by pointer) */
    r.r[1] = (intptr_t)area;
    r.r[2] = (intptr_t)(area + 4);
    r.r[3] = 0;
    if (_kernel_swi(OS_SpriteOp, &r, &out))
        return -1;
    r.r[0] = (intptr_t)jpeg;
    r.r[1] = ((w - tw) / 2) << S.xeig;
    /* taller than the box: most of what's cut comes off the foot, a fifth
       off the head (faces and titles are near the top of posters and
       backdrops; a cut through the middle took off heads) */
    r.r[2] = (th > h ? (h - th) * 4 / 5 : (h - th) / 2) << S.yeig;
    r.r[3] = (intptr_t)scale;
    r.r[4] = (int)len;
    r.r[5] = 1;                     /* dithered */
    e = swi(JPEG_PlotScaled, &r);
    swi(OS_SpriteOp, &out);         /* output back where it was */
    return e ? -1 : 0;
}

/* How much of pixel (x, y) is inside a rounded rectangle w x h with corners
   of radius round: 0 to 16, out of 4 x 4 samples. The samples are at
   eighths of a pixel, so whole numbers in eighths give exactly the same
   answers as the floating point this used to be. */
static int corner_cover(int x, int y, int w, int h, int round)
{
    int r8 = round * 8, n = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            int fx = 8 * x + 2 * i + 1, fy = 8 * y + 2 * j + 1;
            int cx = fx < r8 ? r8 : fx > 8 * w - r8 ? 8 * w - r8 : fx;
            int cy = fy < r8 ? r8 : fy > 8 * h - r8 ? 8 * h - r8 : fy;
            n += (fx - cx) * (fx - cx) + (fy - cy) * (fy - cy) <= r8 * r8;
        }
    return n;
}

/* Rounded corners, round pixels: outside the curve the mask hides the
   pixel; on it, the pixel is mixed with the window's grey by how much of
   it is inside (corner_cover), so the edge is smooth. The four corners
   are mirror images, so (when they don't overlap) the top left one's
   coverage is worked out once and used for all four: a round photo is
   all corner. */
static void sprite_round(int *area, int w, int h, int round)
{
    int *s = area + 4, words = (w + 31) / 32, mirror;
    unsigned *px = (unsigned *)(s + 11), *m = (unsigned *)((char *)s + s[9]);
    unsigned char *cover = NULL;
    const int bg[3] = { 24, 26, 31 };
    if (s[9] == s[8] || round < 1)
        return;
    mirror = 2 * round <= w && 2 * round <= h && (cover = malloc((size_t)round * round)) != NULL;
    if (mirror)
        for (int y = 0; y < round; y++)
            for (int x = 0; x < round; x++)
                cover[y * round + x] = (unsigned char)corner_cover(x, y, w, h, round);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int n;
            if ((x >= round && x < w - round) || (y >= round && y < h - round))
                continue;           /* not in a corner */
            if (mirror)
                n = cover[(y < round ? y : h - 1 - y) * round + (x < round ? x : w - 1 - x)];
            else
                n = corner_cover(x, y, w, h, round);
            if (n < 8) {
                m[y * words + x / 32] &= ~(1u << (x & 31));
            } else if (n < 16) {
                unsigned p = px[y * w + x];
                int c[3] = { (int)(p & 255), (int)(p >> 8 & 255), (int)(p >> 16 & 255) };
                for (int k = 0; k < 3; k++)
                    c[k] = (c[k] * n + bg[k] * (16 - n)) / 16;
                px[y * w + x] = (unsigned)c[0] | (unsigned)c[1] << 8 | (unsigned)c[2] << 16;
            }
        }
    free(cover);
}

/* The backdrop: darkened a little, faded into the window's grey over its
   lower part, and across from the left (where the title, the labels and
   the summary go, so they read as on the grey), as the picture fades into
   the page. Done once, when it's fetched: plotting it costs no more.
   Each pixel is (p * (256 - k) + grey * k) / 256 for every channel, with
   red and blue done together in one word (each product fits in 16 bits,
   so they can't spill into each other) and the grey's part from a table:
   two multiplies a pixel instead of six, the same result. */
static void sprite_fade(int *area, int w, int h)
{
    unsigned *px = (unsigned *)(area + 4 + 11);
    const unsigned bg_rb = 24u | 31u << 16, bg_g = 26u << 8;
    int from = h * 40 / 100, solid = w * 48 / 100, clear = w * 86 / 100;
    static unsigned char across[4096];
    static unsigned grey_rb[257], grey_g[257];
    for (int k = 0; k <= 256; k++) {            /* the grey's part, for each k */
        grey_rb[k] = bg_rb * (unsigned)k;
        grey_g[k] = bg_g * (unsigned)k;
    }
    for (int x = 0; x < w && x < 4096; x++) {       /* how much grey from the left, in 256ths */
        int t = x <= solid ? 0 : x >= clear ? 256 : 256 * (x - solid) / (clear - solid);
        int smooth = t * t / 256 * (768 - 2 * t) / 256;         /* 0..256, easing in and out */
        across[x] = (unsigned char)(236 - 236 * smooth / 256);
    }
    for (int y = 0; y < h; y++) {
        /* down: how much of the grey, in 256ths */
        int kd = y < from ? 40 : 40 + (216 * (y - from) * (y - from)) / ((h - from) * (h - from));
        unsigned *row = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            unsigned p = row[x], keep;
            int ka = x < 4096 ? across[x] : 0, k = kd > ka ? kd : ka;
            keep = (unsigned)(256 - k);
            row[x] = (((p & 0xFF00FFu) * keep + grey_rb[k]) >> 8 & 0xFF00FFu) |
                     (((p & 0xFF00u) * keep + grey_g[k]) >> 8 & 0xFF00u);
        }
    }
}

#ifdef MATINEE_TEST
/* The tests' way in: run them on a picture w x h (0x00BBGGRR) */
void ui_test_sprite_fade(unsigned *pixels, int w, int h)
{
    int *a = calloc(1, 60 + (size_t)w * h * 4);
    memcpy(a + 15, pixels, (size_t)w * h * 4);
    sprite_fade(a, w, h);
    memcpy(pixels, a + 15, (size_t)w * h * 4);
    free(a);
}
/* ... and the mask too: mask (one bit a pixel, rows of (w+31)/32 words) */
void ui_test_sprite_round(unsigned *pixels, unsigned *mask, int w, int h, int round)
{
    size_t image = (size_t)w * h * 4, mbytes = (size_t)(w + 31) / 32 * 4 * h;
    int *a = calloc(1, 60 + image + mbytes);
    a[4 + 8] = 44;
    a[4 + 9] = (int)(44 + image);
    memcpy(a + 15, pixels, image);
    memcpy((char *)(a + 4) + 44 + image, mask, mbytes);
    sprite_round(a, w, h, round);
    memcpy(pixels, a + 15, image);
    memcpy(mask, (char *)(a + 4) + 44 + image, mbytes);
    free(a);
}
#endif

/* A picture from the server (a poster, or a backdrop when art), into the
   cache under key */
/* A picture from the server at w x h pixels: from the disc if it's been
   fetched before, else fetched and kept there (imgcache) */
/* The disc cache's key for a picture at w x h pixels */
static void picture_key(const char *thumb, int w, int h, char *key, size_t size)
{
    snprintf(key, size, "%s|%s|%dx%d", S.px.server_id, thumb, w, h);
}

static int picture_get(const char *thumb, int w, int h, char **jpeg, size_t *len)
{
    char key[600];
    picture_key(thumb, w, h, key, sizeof(key));
    if (imgcache_get(key, jpeg, len) == 0) {
        if (imgcache_jpeg_whole(*jpeg, *len))
            return 0;
        free(*jpeg);                /* cut short (kept by test28 or before): fetched again */
        *jpeg = NULL;
    }
    for (int tries = 0; tries < 2; tries++) {
        if (plex_poster(&S.px, thumb, w, h, jpeg, len) != 0)
            return -1;
        if (imgcache_jpeg_whole(*jpeg, *len)) {
            imgcache_put(key, *jpeg, *len);     /* (it replaces one cut short) */
            return 0;
        }
        if (tries == 0) {           /* cut short on the way: once more */
            free(*jpeg);
            *jpeg = NULL;
        }
    }
    return 0;                       /* the part there is, shown but not kept */
}

/* art: 0 a poster (rounded corners), 1 a backdrop (faded), 2 a person's
   photo (round, filling the circle), 3 an episode's picture (filling its
   box, rounded corners) */
/* The picture made from its JPEG (NULL: none: a picture that failed) and
   put in the cache under key */
static poster_t *poster_make(const char *key, int w, int h, int art, const char *jpeg, size_t len)
{
    int round = art == 2 ? w / 2 : art == 1 ? 0 : 12 >> S.xeig;
    poster_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    snprintf(p->thumb, sizeof(p->thumb), "%s", key);
    p->hash = str_hash(p->thumb);
    if (!jpeg ||
        !(p->area = sprite_make(w, h, round, &p->bytes)) ||
        jpeg_into(p->area, w, h, jpeg, len, art != 0) != 0) {
        free(p->area);
        p->area = NULL;
        p->bytes = sizeof(*p);      /* counted, so failed ones are trimmed too */
        p->failed = 1;
    } else if (art == 1) {
        sprite_fade(p->area, w, h);
    } else {
        sprite_round(p->area, w, h, round);
    }
    p->next = S.cache;
    S.cache = p;
    S.cache_bytes += p->bytes;
    return p;
}

/* A picture fetched now, the desktop waiting (from the disc if it's
   there): for the few a page shows at the top (a backdrop, the details'
   poster) */
poster_t *poster_fetch(const char *thumb, const char *key, int w, int h, int art)
{
    char *jpeg = NULL;
    size_t len = 0;
    poster_t *p;
    if (picture_get(thumb, w, h, &jpeg, &len) != 0) {
        free(jpeg);
        jpeg = NULL;
    }
    p = poster_make(key, w, h, art, jpeg, len);
    free(jpeg);
    return p;
}

/* ---- pictures fetched without waiting -----------------------------------------------

   Posters on the grid and the home page, episodes' pictures, cast photos
   and More like this: each picture is a request to the server (which makes
   it at the size asked for), tens to hundreds of ms. Fetched with
   poster_fetch, the whole desktop waited for each. poster_soon instead
   starts the request (bgreq.c: plain http to an address) and returns at
   once; the null events after it move it on (the Wimp polls in between,
   every 1 cs), and the finished picture goes into the cache, where the
   caller finds it next time. One at a time. A picture on the disc
   (imgcache) is used at once; an https server, or a name, is fetched the
   old way. */

#define PF_TIMEOUT_CS 1500

static struct {
    bgreq r;
    int on;                         /* a picture on its way */
    char key[320];                  /* the cache's key for it */
    char disc_key[600];             /* imgcache's */
    int w, h, art, cs0;
} PF = { .r = { .fd = -1 } };

#ifdef MATINEE_TEST
int ui_test_pictures_waited;        /* pictures fetched the old way, the desktop waiting */
int ui_test_pictures_free;          /* ... and without waiting */
#endif

/* The picture on its way moved on; 1 when it's arrived (or failed: then
   it's in the cache as failed, so it isn't asked for again) */
static int pf_step(void)
{
    int e;
    char *jpeg;
    size_t len;
    if (!PF.on)
        return 0;
    e = bgreq_step(&PF.r);
    if (e == 0 && now_cs() - PF.cs0 < PF_TIMEOUT_CS)
        return 0;
    jpeg = e > 0 && PF.r.status == 200 ? bgreq_take(&PF.r, &len) : NULL;
    bgreq_close(&PF.r);
    PF.on = 0;
    if (jpeg && imgcache_jpeg_whole(jpeg, len))
        imgcache_put(PF.disc_key, jpeg, len);
    poster_make(PF.key, PF.w, PF.h, PF.art, jpeg, jpeg ? len : 0);
    free(jpeg);
    cache_trim();
    return 1;
}

#ifdef MATINEE_TEST
int ui_test_pf_waiting(void) { return PF.on; }
/* The tests' Wimp: while the program sleeps for a picture's answer, the
   time passes for real until it's come, so a script's null events see it
   arrive as they would on a Pi (a little later, never at once) */
void ui_test_pf_settle(void)
{
    for (int i = 0; PF.on && i < 3000; i++) {
        struct timespec ts = { 0, 1000000 };
        if (pf_step())
            break;
        nanosleep(&ts, NULL);
    }
    for (int i = 0; timeline_going() && i < 3000; i++) {      /* and a report on its way */
        struct timespec ts = { 0, 1000000 };
        timeline_step();
        nanosleep(&ts, NULL);
    }
}
#endif

/* The picture on its way forgotten */
static void pf_cancel(void)
{
    bgreq_close(&PF.r);
    PF.on = 0;
}

/* Waiting for a picture's answer (the Wimp can sleep a little meanwhile) */
int pf_waiting(void)
{
    return PF.on;
}

/* The picture for key (as poster_fetch): from the cache or the disc now;
   else NULL, its request started (or still on its way): ask again on a
   later null event */
poster_t *poster_soon(const char *thumb, const char *key, int w, int h, int art)
{
    poster_t *p = cache_find(key);
    plex_request rq;
    char *jpeg = NULL;
    size_t len = 0;
    if (p)
        return p;
    if (PF.on) {
        pf_step();
        return cache_find(key);
    }
    picture_key(thumb, w, h, PF.disc_key, sizeof(PF.disc_key));
    if (imgcache_get(PF.disc_key, &jpeg, &len) == 0) {
        if (imgcache_jpeg_whole(jpeg, len)) {
            p = poster_make(key, w, h, art, jpeg, len);
            free(jpeg);
            return p;
        }
        free(jpeg);                 /* cut short: fetched again */
    }
    if (plex_poster_request(&S.px, thumb, w, h, &rq) == 0 && bgreq_start(&PF.r, rq.url, rq.headers, 1) == 0) {
        PF.on = 1;
        PF.cs0 = now_cs();
        snprintf(PF.key, sizeof(PF.key), "%s", key);
        PF.w = w;
        PF.h = h;
        PF.art = art;
#ifdef MATINEE_TEST
        ui_test_pictures_free++;
#endif
        return NULL;
    }
#ifdef MATINEE_TEST
    ui_test_pictures_waited++;
#endif
    return poster_fetch(thumb, key, w, h, art);
}
