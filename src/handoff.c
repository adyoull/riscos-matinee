/*
 * handoff.c - yt-dlp style JSON for Reel (see handoff.h).
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "handoff.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *handoff_json(const play_t *p, const char *title, const char *user_agent)
{
    cJSON *o = cJSON_CreateObject(), *h = cJSON_CreateObject();
    const char *line = p->headers;
    char *text;
    if (!o || !h) {
        cJSON_Delete(o);
        cJSON_Delete(h);
        return NULL;
    }
    cJSON_AddStringToObject(o, "title", title && *title ? title : "Plex");
    cJSON_AddStringToObject(o, "url", p->url);
    if (*p->key)
        cJSON_AddStringToObject(o, "webpage_url", p->key);
    /* "Name: value\r\n" lines into yt-dlp's http_headers */
    while (line && *line) {
        const char *end = strstr(line, "\r\n"), *colon;
        size_t n = end ? (size_t)(end - line) : strlen(line);
        colon = memchr(line, ':', n);
        if (colon) {
            char name[64], value[512];
            size_t nl = (size_t)(colon - line), vl;
            const char *v = colon + 1;
            while (v < line + n && *v == ' ')
                v++;
            vl = (size_t)(line + n - v);
            if (nl < sizeof(name) && vl < sizeof(value)) {
                memcpy(name, line, nl);
                name[nl] = 0;
                memcpy(value, v, vl);
                value[vl] = 0;
                /* Accept: JSON is for the API, not for the video */
                if (strcmp(name, "Accept"))
                    cJSON_AddStringToObject(h, name, value);
            }
        }
        line = end ? end + 2 : NULL;
    }
    if (user_agent && *user_agent)
        cJSON_AddStringToObject(h, "User-Agent", user_agent);
    cJSON_AddItemToObject(o, "http_headers", h);
    cJSON_AddStringToObject(o, "extractor", "matinee");
    text = cJSON_Print(o);
    cJSON_Delete(o);
    return text;
}

int handoff_write(const char *path, const play_t *p, const char *title, const char *user_agent)
{
    char *text = handoff_json(p, title, user_agent);
    FILE *f;
    int e = 0;
    if (!text)
        return -1;
    if (!(f = fopen(path, "w"))) {
        free(text);
        return -1;
    }
    if (fputs(text, f) < 0 || fputc('\n', f) < 0)
        e = -1;
    if (fclose(f) != 0)
        e = -1;
    free(text);
    return e;
}
