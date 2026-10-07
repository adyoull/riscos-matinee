/*
 * xml.h - a small XML reader, enough for UPnP: a device's description,
 * SOAP answers and the DIDL-Lite lists inside them.
 *
 * The document is read into a tree. Names lose their namespace prefix
 * ("dc:title" is "title", "dlna:profileID" is "profileID"), as UPnP
 * servers differ in the prefixes they use. Entities (&amp; &#233;...) and
 * CDATA are decoded; comments, <?...?> and <!DOCTYPE> are skipped. It
 * doesn't validate: a close tag that doesn't match closes back to the
 * element it names, or is ignored.
 * Part of riscos-matinee. GPL v2 or later.
 */
#ifndef MATINEE_XML_H
#define MATINEE_XML_H

typedef struct xml_attr {
    char *name, *value;
    struct xml_attr *next;
} xml_attr;

typedef struct xml_node {
    char *name;
    xml_attr *attrs;
    char *text;                 /* the element's own text, trimmed ("" if none) */
    struct xml_node *child, *next;
} xml_node;

/* The document's first element (its tree), or NULL if there's none */
xml_node *xml_parse(const char *s);
void xml_free(xml_node *n);

/* The first child called name, or NULL */
const xml_node *xml_child(const xml_node *n, const char *name);
/* The first element called name below n (depth first), or NULL */
const xml_node *xml_find(const xml_node *n, const char *name);
/* A child's text, or NULL if there's no such child */
const char *xml_text(const xml_node *n, const char *child);
/* An attribute's value, or NULL */
const char *xml_attr_get(const xml_node *n, const char *name);

/* s with & < > " escaped, for an XML document (out always ends) */
void xml_escape(const char *s, char *out, unsigned size);

#endif
