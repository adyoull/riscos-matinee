/*
 * xml.c - a small XML reader (see xml.h).
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "xml.h"

#include <stdlib.h>
#include <string.h>

#define DEPTH 64

/* A copy of n bytes with entities decoded */
static char *decode(const char *s, size_t n)
{
    char *o = malloc(n + 1), *p = o;
    const char *e = s + n;
    if (!o)
        return NULL;
    while (s < e) {
        if (*s != '&') {
            *p++ = *s++;
            continue;
        }
        {
            const char *semi = memchr(s, ';', (size_t)(e - s));
            size_t len = semi ? (size_t)(semi - s) : 0;
            unsigned long c = 0;
            if (!semi || len > 10) {
                *p++ = *s++;
                continue;
            }
            if (len == 3 && !strncmp(s, "&lt", 3)) c = '<';
            else if (len == 3 && !strncmp(s, "&gt", 3)) c = '>';
            else if (len == 4 && !strncmp(s, "&amp", 4)) c = '&';
            else if (len == 5 && !strncmp(s, "&quot", 5)) c = '"';
            else if (len == 5 && !strncmp(s, "&apos", 5)) c = '\'';
            else if (len > 2 && s[1] == '#')
                c = s[2] == 'x' || s[2] == 'X' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
            if (!c) {               /* not one we know: kept as it is */
                *p++ = *s++;
                continue;
            }
            /* as UTF-8 */
            if (c < 0x80) {
                *p++ = (char)c;
            } else if (c < 0x800) {
                *p++ = (char)(0xC0 | c >> 6);
                *p++ = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                if (len < 3) {      /* room: "&#x" at least 4 bytes in, 3 out */
                    *p++ = '?';
                } else {
                    *p++ = (char)(0xE0 | c >> 12);
                    *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
                    *p++ = (char)(0x80 | (c & 0x3F));
                }
            } else {
                *p++ = '?';
            }
            s = semi + 1;
        }
    }
    *p = 0;
    return o;
}

/* The local name (after any prefix) of n bytes, as a string */
static char *local_name(const char *s, size_t n)
{
    const char *c = memchr(s, ':', n);
    char *o;
    if (c) {
        n -= (size_t)(c + 1 - s);
        s = c + 1;
    }
    o = malloc(n + 1);
    if (o) {
        memcpy(o, s, n);
        o[n] = 0;
    }
    return o;
}

/* Adds n bytes of text to node's */
static void add_text(xml_node *node, const char *s, size_t n, int raw)
{
    char *t = raw ? NULL : decode(s, n), *j;
    const char *add = raw ? s : t;
    size_t an = raw ? n : (t ? strlen(t) : 0), old = node->text ? strlen(node->text) : 0;
    if (!raw && !t)
        return;
    j = realloc(node->text, old + an + 1);
    if (j) {
        memcpy(j + old, add, an);
        j[old + an] = 0;
        node->text = j;
    }
    free(t);
}

static int space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void trim(char *s)
{
    size_t n, a = 0;
    if (!s)
        return;
    while (space(s[a]))
        a++;
    n = strlen(s + a);
    while (n && space(s[a + n - 1]))
        n--;
    memmove(s, s + a, n);
    s[n] = 0;
}

static void finish(xml_node *n)
{
    if (!n->text)
        n->text = calloc(1, 1);
    else
        trim(n->text);
}

xml_node *xml_parse(const char *s)
{
    xml_node *stack[DEPTH], *root = NULL, *last[DEPTH];
    int depth = 0;
    if (!s)
        return NULL;
    while (*s) {
        if (*s != '<') {
            const char *e = strchr(s, '<');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            if (depth)
                add_text(stack[depth - 1], s, n, 0);
            s += n;
            continue;
        }
        if (!strncmp(s, "<!--", 4)) {
            const char *e = strstr(s + 4, "-->");
            if (!e)
                break;
            s = e + 3;
            continue;
        }
        if (!strncmp(s, "<![CDATA[", 9)) {
            const char *e = strstr(s + 9, "]]>");
            if (!e)
                break;
            if (depth)
                add_text(stack[depth - 1], s + 9, (size_t)(e - s - 9), 1);
            s = e + 3;
            continue;
        }
        if (s[1] == '?' || s[1] == '!') {
            const char *e = strchr(s, '>');
            if (!e)
                break;
            s = e + 1;
            continue;
        }
        if (s[1] == '/') {          /* a close tag: back to the element it names */
            const char *e = strchr(s, '>');
            char *nm;
            if (!e)
                break;
            nm = local_name(s + 2, strcspn(s + 2, " \t\r\n>"));
            for (int d = depth - 1; nm && d >= 0; d--)
                if (!strcmp(stack[d]->name, nm)) {
                    while (depth > d)
                        finish(stack[--depth]);
                    break;
                }
            free(nm);
            s = e + 1;
            continue;
        }
        {                           /* an element */
            xml_node *n = calloc(1, sizeof(*n));
            xml_attr **ap;
            size_t len = strcspn(s + 1, " \t\r\n/>");
            int empty = 0;
            if (!n)
                break;
            n->name = local_name(s + 1, len);
            s += 1 + len;
            ap = &n->attrs;
            for (;;) {              /* its attributes */
                const char *an;
                size_t al;
                char q;
                while (space(*s))
                    s++;
                if (!*s)
                    break;
                if (*s == '/') {
                    empty = 1;
                    s++;
                    continue;
                }
                if (*s == '>') {
                    s++;
                    break;
                }
                an = s;
                al = strcspn(s, " \t\r\n=/>");
                s += al;
                while (space(*s))
                    s++;
                if (*s != '=') {    /* a name with no value */
                    if (!al)
                        s++;
                    continue;
                }
                s++;
                while (space(*s))
                    s++;
                q = *s;
                if (q == '"' || q == '\'') {
                    const char *e = strchr(s + 1, q);
                    xml_attr *a;
                    if (!e) {
                        s += strlen(s);
                        break;
                    }
                    a = calloc(1, sizeof(*a));
                    if (a) {
                        a->name = local_name(an, al);
                        a->value = decode(s + 1, (size_t)(e - s - 1));
                        *ap = a;
                        ap = &a->next;
                    }
                    s = e + 1;
                } else {            /* unquoted: up to a space or the end */
                    s += strcspn(s, " \t\r\n>");
                }
            }
            if (!n->name) {
                xml_free(n);
                break;
            }
            if (!depth) {
                if (root) {         /* one root only: anything after it is ignored */
                    xml_free(n);
                    break;
                }
                root = n;
            } else {
                xml_node *p = stack[depth - 1];
                if (!p->child)
                    p->child = n;
                else
                    last[depth - 1]->next = n;
                last[depth - 1] = n;
            }
            if (empty || depth >= DEPTH) {
                finish(n);
            } else {
                stack[depth] = n;
                last[depth] = NULL;
                depth++;
            }
        }
    }
    while (depth)
        finish(stack[--depth]);
    return root;
}

void xml_free(xml_node *n)
{
    while (n) {
        xml_node *next = n->next;
        xml_attr *a = n->attrs;
        while (a) {
            xml_attr *an = a->next;
            free(a->name);
            free(a->value);
            free(a);
            a = an;
        }
        xml_free(n->child);
        free(n->name);
        free(n->text);
        free(n);
        n = next;
    }
}

const xml_node *xml_child(const xml_node *n, const char *name)
{
    for (n = n ? n->child : NULL; n; n = n->next)
        if (!strcmp(n->name, name))
            return n;
    return NULL;
}

const xml_node *xml_find(const xml_node *n, const char *name)
{
    for (n = n ? n->child : NULL; n; n = n->next) {
        const xml_node *f;
        if (!strcmp(n->name, name))
            return n;
        if ((f = xml_find(n, name)) != NULL)
            return f;
    }
    return NULL;
}

const char *xml_text(const xml_node *n, const char *child)
{
    const xml_node *c = xml_child(n, child);
    return c ? c->text : NULL;
}

const char *xml_attr_get(const xml_node *n, const char *name)
{
    for (const xml_attr *a = n ? n->attrs : NULL; a; a = a->next)
        if (!strcmp(a->name, name))
            return a->value;
    return NULL;
}

void xml_escape(const char *s, char *out, unsigned size)
{
    unsigned o = 0;
    if (!size)
        return;
    for (; s && *s; s++) {
        const char *r = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : NULL;
        unsigned n = r ? (unsigned)strlen(r) : 1;
        if (o + n + 1 > size)
            break;
        if (r)
            memcpy(out + o, r, n);
        else
            out[o] = *s;
        o += n;
    }
    out[o] = 0;
}
