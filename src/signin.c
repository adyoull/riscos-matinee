/*
 * signin.c - the sign-in page (Plex, Jellyfin and DLNA), and the servers: choosing
 * one, adding one, forgetting one, signing out.
 * Part of riscos-matinee. GPL v2 or later.
 */
#include "ui_int.h"

/* ---- sign-in: a page of the window ------------------------------------------------- */

/* Before a server is chosen the window shows the sign-in page. Plex's: the
   code to type at plex.tv/link (checked every 2 seconds), New code, and a
   server's address and token for signing in without plex.tv; and
   "Jellyfin server..." for the other half. Jellyfin's: the server's
   address, Quick Connect (a code to type into Jellyfin, checked every 2
   seconds), or a name and password; and "Plex instead". The fields are
   drawn here and typed into here (the window has the caret): Tab or the
   arrow keys move between them, Return goes on. */

static void jf_signed_in(void);

si_box_t si_box[SI_MAX];
int si_n;

static void si_add_h(int id, int x0, int y1, int w, int h)
{
    if (si_n >= SI_MAX)
        return;
    si_box[si_n].id = id;
    si_box[si_n].x0 = x0;
    si_box[si_n].x1 = x0 + w;
    si_box[si_n].y1 = y1;
    si_box[si_n].y0 = y1 - h;
    si_n++;
}

static void si_add(int id, int x0, int y1, int w)
{
    if (si_n >= SI_MAX)
        return;
    si_box[si_n].id = id;
    si_box[si_n].x0 = x0;
    si_box[si_n].x1 = x0 + w;
    si_box[si_n].y1 = y1;
    si_box[si_n].y0 = y1 - BTN;
    si_n++;
}

static const char *si_label(int id)
{
    switch (id) {
    case S_NEWCODE: return "New code";
    case S_USE:     return "Use these";
    case S_PICK_PLEX: return "Plex";
    case S_PICK_JF: return "Jellyfin";
    case S_PICK_DLNA: return "DLNA";
    case S_DUSE:    return "Use";
    case S_DSEARCH: return "Search again";
    case S_BACK:    return "Back";
    case S_CANCEL:  return "Cancel";
    case S_QC:      return S.jf_secret[0] ? "New code" : "Quick Connect";
    case S_LOGIN:   return "Sign in";
    }
    return "";
}

static int si_bw(int id)
{
    return draw_width(D_BOLD, si_label(id)) + 64;
}

/* The DLNA page: where the part for an address starts, below the servers found */
static int dl_below(void)
{
    int n = S.nfound < SI_FOUND_SHOWN ? S.nfound : SI_FOUND_SHOWN;
    return -HEADER_H - 160 - (n ? n : 1) * 84;
}

void si_layout(void)
{
    int top = -HEADER_H;
    si_n = 0;
    if (S.si_mode == SI_PICK) {         /* three cards, then Cancel */
        si_add_h(S_PICK_PLEX, 40, top - 176, 1160, 150);
        si_add_h(S_PICK_JF, 40, top - 360, 1160, 150);
        si_add_h(S_PICK_DLNA, 40, top - 544, 1160, 150);
        si_add(S_CANCEL, 40, top - 744, si_bw(S_CANCEL));
    } else if (S.si_mode == SI_DLNA) {  /* the servers found, Search again; an address, Use, Back, Cancel */
        int y = dl_below(), n = S.nfound < SI_FOUND_SHOWN ? S.nfound : SI_FOUND_SHOWN;
        for (int i = 0; i < n; i++)
            si_add_h(S_FOUND + i, 40, top - 160 - i * 84, 1160, 72);
        si_add(S_DSEARCH, 40, y - 16, si_bw(S_DSEARCH));
        si_add(S_DADDR, 220, y - 200, 620);
        si_add(S_DUSE, 220, y - 284, si_bw(S_DUSE));
        si_add(S_BACK, 220 + si_bw(S_DUSE) + 32, y - 284, si_bw(S_BACK));
        si_add(S_CANCEL, 220 + si_bw(S_DUSE) + si_bw(S_BACK) + 64, y - 284, si_bw(S_CANCEL));
    } else if (S.si_mode == SI_PLEX) {
        si_add(S_NEWCODE, 40, top - 372, si_bw(S_NEWCODE));
        si_add(S_ADDR, 220, top - 572, 620);
        si_add(S_TOK, 220, top - 652, 620);
        si_add(S_USE, 220, top - 736, si_bw(S_USE));
        si_add(S_BACK, 220 + si_bw(S_USE) + 32, top - 736, si_bw(S_BACK));
        si_add(S_CANCEL, 220 + si_bw(S_USE) + si_bw(S_BACK) + 64, top - 736, si_bw(S_CANCEL));
    } else {
        si_add(S_JADDR, 220, top - 132, 620);
        si_add(S_QC, 40, top - 372, si_bw(S_QC));
        si_add(S_JUSER, 220, top - 572, 620);
        si_add(S_JPW, 220, top - 652, 620);
        si_add(S_LOGIN, 220, top - 736, si_bw(S_LOGIN));
        si_add(S_BACK, 220 + si_bw(S_LOGIN) + 32, top - 736, si_bw(S_BACK));
        si_add(S_CANCEL, 220 + si_bw(S_LOGIN) + si_bw(S_BACK) + 64, top - 736, si_bw(S_CANCEL));
    }
}

int si_hit(int sx, int sy)
{
    int st[9], wx, wy;
    window_state(S.browser_w, st);
    wx = sx - (st[1] - st[5]);
    wy = sy - (st[4] - st[6]);
    si_layout();
    for (int i = 0; i < si_n; i++)
        if (wx >= si_box[i].x0 && wx < si_box[i].x1 && wy >= si_box[i].y0 && wy < si_box[i].y1)
            return si_box[i].id;
    return 0;
}

/* The fields: their text, and which is which (the caret's S.field) */
int si_field_of(int id)
{
    switch (id) {
    case S_ADDR: case S_JADDR: case S_DADDR: return 0;
    case S_TOK: case S_JUSER:  return 1;
    case S_JPW:                return 2;
    }
    return -1;
}

static char *si_field(int f, size_t *size)
{
    char *p;
    if (S.si_mode == SI_DLNA) {
        *size = sizeof(S.dl_addr);
        return S.dl_addr;
    }
    if (S.si_mode != SI_JF) {
        p = f ? S.si_tok : S.si_addr;
        *size = f ? sizeof(S.si_tok) : sizeof(S.si_addr);
    } else {
        p = f == 0 ? S.jf_addr : f == 1 ? S.jf_user : S.jf_pw;
        *size = f == 0 ? sizeof(S.jf_addr) : f == 1 ? sizeof(S.jf_user) : sizeof(S.jf_pw);
    }
    return p;
}

static int si_fields(void)
{
    return S.si_mode == SI_JF ? 3 : S.si_mode == SI_PLEX ? 2 : S.si_mode == SI_DLNA ? 1 : 0;
}

/* The code, big and spaced out, on a card; the card's right edge (work area) */
static int si_code(int ox, int oy, const char *code)
{
    int top = -HEADER_H;
    char t[64];
    size_t n = 0;
    for (const char *c = code; *c && n + 3 < sizeof(t); c++) {
        t[n++] = *c;
        if (c[1]) { t[n++] = ' '; t[n++] = ' '; }
    }
    t[n] = 0;
    int w = draw_width(D_TITLE, t) + 120;
    if (w < 420)                    /* Plex's four letters; Jellyfin's six digits wider */
        w = 420;
    draw_round(ox + 40, oy + top - 292, ox + 40 + w, oy + top - 160, 20, C_CARD, C_BG);
    draw_text(D_TITLE, ox + 40 + (w - draw_width(D_TITLE, t)) / 2, oy + top - 244, t, C_TEXT, C_CARD);
    return 40 + w;
}

void signin_redraw(int ox, int oy, int vis_w)
{
    int top = -HEADER_H;
    char t[300];
    (void)vis_w;
    si_layout();
    if (S.si_mode == SI_PICK) {
        draw_text(D_BOLD, ox + 40, oy + top - 76, "Add a server", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 124, "Which kind is it?", C_SUB, C_BG);
    } else if (S.si_mode == SI_DLNA) {
        int y = dl_below();
        draw_text(D_BOLD, ox + 40, oy + top - 76, "Add a DLNA server", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 124,
                  S.nfound ? "On your network (click one to use it):" :
                  S.searched ? "None found on your network." : "Looking on your network...", C_SUB, C_BG);
        draw_text(D_BODY, ox + 40 + si_bw(S_DSEARCH) + 32, oy + y - 58, S.si_status, C_SUB, C_BG);
        draw_rect(ox + 40, oy + y - 108, ox + 1000, oy + y - 106, C_CARD);
        draw_text(D_BOLD, ox + 40, oy + y - 160, "Or its address", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + y - 242, "Address", C_SUB, C_BG);
        draw_text(D_BODY, ox + 220, oy + y - 400, "As 192.168.1.10:8200 (MiniDLNA), :32469 (Plex), or its description's",
                  C_SUB, C_BG);
        draw_text(D_BODY, ox + 220, oy + y - 440, "address (http://.../rootDesc.xml).", C_SUB, C_BG);
    } else if (S.si_mode == SI_PLEX) {
        draw_text(D_BOLD, ox + 40, oy + top - 76, "Sign in to Plex with a code", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 124, "On a phone or computer, go to plex.tv/link and type this code:",
                  C_SUB, C_BG);
        {
            int x1 = si_code(ox, oy, S.code);
            draw_text(D_BODY, ox + x1 + 40, oy + top - 244, S.si_status, C_SUB, C_BG);
        }
        draw_rect(ox + 40, oy + top - 468, ox + 1000, oy + top - 466, C_CARD);
        draw_text(D_BOLD, ox + 40, oy + top - 520, "Or a server on your network", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 616, "Address", C_SUB, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 696, "Token", C_SUB, C_BG);
    } else {
        draw_text(D_BOLD, ox + 40, oy + top - 76, "Sign in to a Jellyfin server", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 176, "Address", C_SUB, C_BG);
        if (S.jf_secret[0]) {
            si_code(ox, oy - 60, S.jf_code);
            draw_text(D_BODY, ox + 40 + si_bw(S_QC) + 32, oy + top - 412, S.si_status, C_SUB, C_BG);
        } else {
            draw_text(D_BODY, ox + 40, oy + top - 236,
                      "Quick Connect gives a code to type into Jellyfin on a phone or computer", C_SUB, C_BG);
            draw_text(D_BODY, ox + 40, oy + top - 284, "(its user menu, then Quick Connect).", C_SUB, C_BG);
            draw_text(D_BODY, ox + 40 + si_bw(S_QC) + 32, oy + top - 412, S.si_status, C_SUB, C_BG);
        }
        draw_rect(ox + 40, oy + top - 468, ox + 1000, oy + top - 466, C_CARD);
        draw_text(D_BOLD, ox + 40, oy + top - 520, "Or your name and password", C_TEXT, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 616, "Name", C_SUB, C_BG);
        draw_text(D_BODY, ox + 40, oy + top - 696, "Password", C_SUB, C_BG);
    }
    for (int i = 0; i < si_n; i++) {
        int x0 = ox + si_box[i].x0, y0 = oy + si_box[i].y0, x1 = ox + si_box[i].x1, y1 = oy + si_box[i].y1;
        int f = si_field_of(si_box[i].id);
        if (f >= 0) {
            size_t size;
            const char *v = si_field(f, &size);
            int tx = x0 + 20;
            if (si_box[i].id == S_TOK || si_box[i].id == S_JPW) {   /* shown as dots */
                size_t n = strlen(v);
                if (n > 60) n = 60;
                memset(t, 0xB7, n);
                t[n] = 0;
            } else {
                snprintf(t, sizeof(t), "%s", v);
            }
            if (S.field == f)       /* the caret's field: a blue edge */
                draw_round(x0 - 4, y0 - 4, x1 + 4, y1 + 4, 16, C_ACCENT, C_BG);
            draw_round(x0, y0, x1, y1, 12, C_CARD, S.field == f ? C_ACCENT : C_BG);
            draw_text(D_BODY, tx, y0 + 22, t, C_TEXT, C_CARD);
            if (S.field == f) {     /* the caret */
                int cx = tx + draw_width(D_BODY, t) + 2;
                draw_rect(cx, y0 + 14, cx + 4, y1 - 14, C_ACCENT);
            }
        } else if (si_box[i].id == S_PICK_PLEX || si_box[i].id == S_PICK_JF || si_box[i].id == S_PICK_DLNA) {
            int id = si_box[i].id;
            draw_round(x0, y0, x1, y1, 20, C_CARD, C_BG);
            draw_text(D_TITLE, x0 + 40, y1 - 70, si_label(id), C_TEXT, C_CARD);
            draw_text(D_BODY, x0 + 40, y0 + 28,
                      id == S_PICK_JF ? "Quick Connect, or your name and password" :
                      id == S_PICK_DLNA ? "On your network: MiniDLNA, a NAS, Plex's or Jellyfin's" :
                           "A code at plex.tv/link, or a server's address and token", C_SUB, C_CARD);
        } else if (si_box[i].id >= S_FOUND && si_box[i].id < S_FOUND + SI_FOUND_SHOWN) {
            const dlna_server *d = &S.found[si_box[i].id - S_FOUND];
            char nm[80];
            latin1(d->name, nm, sizeof(nm));
            draw_round(x0, y0, x1, y1, 16, C_CARD, C_BG);
            draw_text(D_BOLD, x0 + 32, y0 + 22, nm, C_TEXT, C_CARD);
            draw_text(D_BODY, x0 + 640, y0 + 22, d->host, C_SUB, C_CARD);
        } else {
            int id = si_box[i].id;
            unsigned bg = id == S_USE || id == S_LOGIN || id == S_DUSE ? C_ACCENT : C_CARD;
            draw_round(x0, y0, x1, y1, BTN / 2, bg, C_BG);
            draw_text(D_BOLD, x0 + 32, y0 + 22, si_label(id), C_TEXT, bg);
        }
    }
}

void si_redraw_fields(void)
{
    if (S.browser_open && S.page == PG_SIGNIN)
        force_redraw(S.browser_w, 0, S.si_mode == SI_DLNA ? -HEADER_H - 1400 : -HEADER_H - 740, S.scr_w,
                     S.si_mode == SI_JF || S.si_mode == SI_DLNA ? -HEADER_H : -HEADER_H - 540);
}

void pin_new(void)
{
    char code[16];
    long long id;
    hourglass(1);
    if (plex_pin_create(&S.px, &id, code, sizeof(code)) != 0) {
        hourglass(0);
        S.pin_id = 0;
        snprintf(S.code, sizeof(S.code), "-");
        si_set_status("Can't reach plex.tv: %s", S.px.err);
        return;
    }
    hourglass(0);
    S.pin_id = id;
    S.pin_next = now_cs() + PIN_EVERY;
    snprintf(S.code, sizeof(S.code), "%s", code);
    si_set_status("Waiting for the code...");
}

/* The window, showing the sign-in page: the choice of kind (SI_PICK),
   Plex's (SI_PLEX) or Jellyfin's (SI_JF). The page it was on is kept for
   Cancel (playing stops first). */
void signin_show(int mode)
{
    int st[9];
    if (S.page == PG_PLAYER)
        builtin_stop(1);
    if (S.page != PG_SIGNIN) {
        S.si_prev = S.page;
        if (S.page == PG_GRID && S.browser_open) {
            window_state(S.browser_w, st);
            S.grid_sy = st[6];      /* where the grid was scrolled to */
        }
    }
    S.page = PG_SIGNIN;
    S.si_mode = mode;
    S.field = 0;
    S.hover = -1;
    S.posters_wanted = 0;
    if (!S.browser_open)
        browser_open();
    set_where();
    set_extent();
    set_status(mode == SI_JF ? "Sign in to a Jellyfin server." : mode == SI_PLEX ? "Sign in to Plex." :
               mode == SI_DLNA ? "Add a DLNA server." : "Add a Plex, a Jellyfin or a DLNA server.");
    if (mode == SI_DLNA)
        S.si_status[0] = 0;
    window_state(S.browser_w, st);
    open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, 0);
    force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
    set_caret(S.browser_w, -1, NULL);
    if (mode != SI_PLEX)
        S.pin_id = 0;               /* plex.tv isn't asked meanwhile */
    if (mode != SI_JF)
        S.jf_secret[0] = 0;
    if (mode == SI_PLEX && !S.pin_id)
        pin_new();
    if (mode == SI_JF && !S.jf_secret[0])
        si_set_status("Type the address first.");
}

void signin_open(void)
{
    signin_show(SI_PICK);
}

void signin_close(void)
{
    S.pin_id = 0;                   /* no more checks */
    S.jf_secret[0] = 0;
    S.jf_pw[0] = 0;
}

/* Cancel: no server added; back to the page before (the server in use's),
   or the window closed if there's none */
void signin_cancel(void)
{
    int st[9];
    signin_close();
    if (!*S.px.base) {
        if (S.browser_open)
            close_window(S.browser_w);
        S.browser_open = 0;
        S.page = PG_GRID;
        return;
    }
    if (!S.have_list || (S.si_prev == PG_DETAILS && !S.have_det)) {
        browser_top();
        return;
    }
    S.page = S.si_prev == PG_DETAILS ? PG_DETAILS : PG_GRID;
    set_extent();
    set_where();
    set_status("%s", S.px.server_name);
    window_state(S.browser_w, st);
    open_front(S.browser_w, st[1], st[2], st[3], st[4], 0, S.page == PG_GRID ? S.grid_sy : 0);
    force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
    if (S.page == PG_DETAILS)
        det_refresh();
    S.posters_wanted = S.page == PG_GRID;
}

/* A key on the sign-in page: 1 if it was used; 2 Use these (Plex), 3 Sign in (Jellyfin), 4 Cancel,
   5 Use (DLNA) */
int signin_key(int k)
{
    size_t size, n;
    char *f;
    if (k == 0x1B)
        return 4;                   /* Escape: Cancel */
    if (S.si_mode == SI_PICK)
        return 0;
    f = si_field(S.field, &size);
    n = strlen(f);
    if ((k >= 32 && k < 127) || (k >= 160 && k < 256)) {
        if (n + 1 < size) {
            f[n] = (char)k;
            f[n + 1] = 0;
        }
    } else if (k == 8 || k == 0x7F) {
        if (n)
            f[n - 1] = 0;
    } else if (k == 21) {           /* Ctrl-U: empty it */
        f[0] = 0;
    } else if (k == 9 || k == 0x18E) {
        S.field = (S.field + 1) % si_fields();
    } else if (k == 0x18F) {
        S.field = (S.field + si_fields() - 1) % si_fields();
    } else if (k == 13) {
        if (S.field + 1 < si_fields())
            S.field++;
        else
            return S.si_mode == SI_JF ? 3 : S.si_mode == SI_DLNA ? 5 : 2;
    } else {
        return 0;
    }
    si_redraw_fields();
    return 1;
}

void pin_check(void)
{
    int e = plex_pin_check(&S.px, S.pin_id);
    S.pin_next = now_cs() + PIN_EVERY;
    if (e == 0)
        return;
    if (e < 0) {
        S.pin_id = 0;
        si_set_status("The code has run out: click New code for another.");
        return;
    }
    S.pin_id = 0;
    S.nservers = 0;
    choices_save();
    si_set_status("Signed in.");
    signin_close();
    choose_server();
}

void use_manual(void)
{
    plex_ctx c = S.px;
    char *a = S.si_addr;
    a[strcspn(a, "\r\n")] = 0;
    S.si_tok[strcspn(S.si_tok, "\r\n")] = 0;
    if (!*a) {
        si_set_status("Type the server's address first (such as 192.168.1.10).");
        S.field = 0;
        si_redraw_fields();
        return;
    }
    si_set_status("Trying %s...", a);
    hourglass(1);
    if (plex_use_address(&c, a, S.si_tok) != 0) {
        hourglass(0);
        si_set_status("%s", c.err);
        return;
    }
    hourglass(0);
    builtin_stop(0);
    S.px = c;
    choices_save();
    signin_close();
    browser_top();
}

/* ---- signing in to Jellyfin ------------------------------------------------------------ */

/* The Jellyfin server at the address typed (S.jfc), if it isn't already */
static int jf_reach(void)
{
    char *a = S.jf_addr;
    a[strcspn(a, "\r\n")] = 0;
    if (!*a) {
        si_set_status("Type the server's address first (such as 192.168.1.10).");
        S.field = 0;
        si_redraw_fields();
        return -1;
    }
    S.jfc = S.px;                   /* who this computer is */
    si_set_status("Trying %s...", a);
    hourglass(1);
    if (jf_connect(&S.jfc, a) != 0) {
        hourglass(0);
        si_set_status("%s", S.jfc.err);
        return -1;
    }
    hourglass(0);
    return 0;
}

void qc_new(void)
{
    char secret[96], code[16];
    int e;
    S.jf_secret[0] = 0;
    if (jf_reach() != 0)
        return;
    hourglass(1);
    e = jf_qc_start(&S.jfc, secret, sizeof(secret), code, sizeof(code));
    hourglass(0);
    if (e != 0) {
        si_set_status(e == -2 ? "Quick Connect is off on %s: sign in with your name and password."
                              : "Can't start Quick Connect: %s", e == -2 ? S.jfc.server_name : S.jfc.err);
        return;
    }
    snprintf(S.jf_secret, sizeof(S.jf_secret), "%s", secret);
    snprintf(S.jf_code, sizeof(S.jf_code), "%s", code);
    S.qc_next = now_cs() + PIN_EVERY;
    si_set_status("Type the code in Jellyfin: Quick Connect.");
    if (S.browser_open && S.page == PG_SIGNIN)
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
}

void qc_check(void)
{
    int e = jf_qc_check(&S.jfc, S.jf_secret);
    S.qc_next = now_cs() + PIN_EVERY;
    if (e == 0)
        return;
    if (e < 0) {
        S.jf_secret[0] = 0;
        si_set_status("The code has run out: click Quick Connect for another.");
        return;
    }
    jf_signed_in();
}

void jf_password(void)
{
    int e;
    if (jf_reach() != 0)
        return;
    if (!*S.jf_user) {
        si_set_status("Type your name on %s.", S.jfc.server_name);
        S.field = 1;
        si_redraw_fields();
        return;
    }
    hourglass(1);
    e = jf_login(&S.jfc, S.jf_user, S.jf_pw);
    hourglass(0);
    if (e != 0) {
        si_set_status(e == -2 ? "%s didn't take that name and password." : "Can't sign in: %s",
                      e == -2 ? S.jfc.server_name : S.jfc.err);
        S.jf_pw[0] = 0;
        S.field = 2;
        si_redraw_fields();
        return;
    }
    jf_signed_in();
}

/* ---- servers -------------------------------------------------------------------- */

static void server_changed(void);

int servers_get(void)
{
    int n;
    hourglass(1);
    n = plex_servers(&S.px, S.servers, SERVERS_MAX);
    hourglass(0);
    if (n < 0) {
        report("Can't get your servers: %s", S.px.err);
        S.nservers = 0;
        return -1;
    }
    S.nservers = n;
    return n;
}

int use_server(int i)
{
    plex_ctx c = S.px;
    set_status("Trying %s...", S.servers[i].name);
    hourglass(1);
    if (plex_use_server(&c, &S.servers[i]) != 0) {
        hourglass(0);
        report("Can't reach %s: %s", S.servers[i].name, c.err);
        return -1;
    }
    hourglass(0);
    if (S.px.kind != SRV_PLEX || strcmp(S.px.server_id, c.server_id))
        server_changed();
    S.px = c;
    choices_save();
    return 0;
}

/* After signing in: the server (the one you own first), then the browser */
void choose_server(void)
{
    int best = 0;
    if (servers_get() <= 0) {
        if (S.nservers == 0 && !*S.px.err)
            report("Your Plex account has no servers.");
        return;
    }
    for (int i = 0; i < S.nservers; i++)
        if (S.servers[i].owned) {
            best = i;
            break;
        }
    if (use_server(best) == 0)
        browser_top();
}

/* ---- Jellyfin servers -------------------------------------------------------------- */

/* Choices' line can't take a '|' in a name */
static void no_bars(char *s)
{
    for (; *s; s++)
        if (*s == '|' || *s == '\n' || *s == '\r')
            *s = '/';
}

/* The server in use (a Jellyfin one) kept, the same server and user replaced */
static void jf_remember(void)
{
    jf_saved *j = NULL;
    for (int i = 0; i < S.njf; i++)
        if (!strcmp(S.jf[i].id, S.px.server_id) && !strcmp(S.jf[i].uid, S.px.user_id))
            j = &S.jf[i];
    if (!j) {
        if (S.njf >= JF_SAVED) {        /* the oldest makes room */
            memmove(&S.jf[0], &S.jf[1], (JF_SAVED - 1) * sizeof(jf_saved));
            S.njf--;
        }
        j = &S.jf[S.njf++];
    }
    snprintf(j->name, sizeof(j->name), "%s", S.px.server_name);
    snprintf(j->id, sizeof(j->id), "%s", S.px.server_id);
    snprintf(j->base, sizeof(j->base), "%s", S.px.base);
    snprintf(j->token, sizeof(j->token), "%s", S.px.token);
    snprintf(j->uid, sizeof(j->uid), "%s", S.px.user_id);
    snprintf(j->uname, sizeof(j->uname), "%s", S.px.user_name);
    no_bars(j->name);
    no_bars(j->uname);
}

/* Moving to another server: what was shown of the last one goes */
static void server_changed(void)
{
    builtin_stop(1);                /* off the player page too */
    if (S.libs.n || S.libs.v)
        plex_list_free(&S.libs);
    S.nhist = 0;
}

/* The server (a copy of the context, kind and all) made the one in use;
   the Plex account's sign-in stays */
static void server_take(const plex_ctx *c)
{
    plex_ctx *d = &S.px;
    d->kind = c->kind;
    d->local = c->local;
    snprintf(d->base, sizeof(d->base), "%s", c->base);
    snprintf(d->token, sizeof(d->token), "%s", c->token);
    snprintf(d->server_name, sizeof(d->server_name), "%s", c->server_name);
    snprintf(d->server_id, sizeof(d->server_id), "%s", c->server_id);
    snprintf(d->user_id, sizeof(d->user_id), "%s", c->user_id);
    snprintf(d->user_name, sizeof(d->user_name), "%s", c->user_name);
    snprintf(d->ctl, sizeof(d->ctl), "%s", c->ctl);
    snprintf(d->svc, sizeof(d->svc), "%s", c->svc);
}

/* Signed in on the sign-in page's Jellyfin half: that server, then its home page */
static void jf_signed_in(void)
{
    server_changed();
    server_take(&S.jfc);
    jf_remember();
    choices_save();
    signin_close();
    browser_top();
    set_status("Signed in to %s as %s.", S.px.server_name, S.px.user_name);
}

/* Jellyfin server k of the saved ones */
int use_jf(int k)
{
    plex_ctx c = S.px;
    if (k < 0 || k >= S.njf)
        return -1;
    c.kind = SRV_JELLYFIN;
    c.local = 1;
    snprintf(c.base, sizeof(c.base), "%s", S.jf[k].base);
    snprintf(c.token, sizeof(c.token), "%s", S.jf[k].token);
    snprintf(c.server_name, sizeof(c.server_name), "%s", S.jf[k].name);
    snprintf(c.server_id, sizeof(c.server_id), "%s", S.jf[k].id);
    snprintf(c.user_id, sizeof(c.user_id), "%s", S.jf[k].uid);
    snprintf(c.user_name, sizeof(c.user_name), "%s", S.jf[k].uname);
    server_changed();
    server_take(&c);
    choices_save();
    browser_top();
    return 0;
}

/* The Jellyfin page of the sign-in, with the address of the server in
   use (or the last one) to start from */
void jf_add(void)
{
    if (!*S.jf_addr) {
        const char *b = S.px.kind == SRV_JELLYFIN ? S.px.base : S.njf ? S.jf[S.njf - 1].base : "";
        snprintf(S.jf_addr, sizeof(S.jf_addr), "%s", b);
    }
    S.jf_secret[0] = 0;
    signin_show(SI_JF);
}

/* ---- DLNA servers ------------------------------------------------------------------ */

/* The server in use (a DLNA one) kept, the same server replaced */
static void dl_remember(void)
{
    dl_saved *d = NULL;
    for (int i = 0; i < S.ndl; i++)
        if (!strcmp(S.dl[i].udn, S.px.server_id))
            d = &S.dl[i];
    if (!d) {
        if (S.ndl >= DL_SAVED) {        /* the oldest makes room */
            memmove(&S.dl[0], &S.dl[1], (DL_SAVED - 1) * sizeof(dl_saved));
            S.ndl--;
        }
        d = &S.dl[S.ndl++];
    }
    snprintf(d->name, sizeof(d->name), "%s", S.px.server_name);
    snprintf(d->udn, sizeof(d->udn), "%s", S.px.server_id);
    snprintf(d->loc, sizeof(d->loc), "%s", S.px.base);
    no_bars(d->name);
}

/* The servers on the network looked for (2 s), and the page drawn again */
void dl_search(void)
{
    char err[256];
    S.si_status[0] = 0;
    hourglass(1);
    S.nfound = dlna_discover(S.found, DLNA_FOUND, 2000, err, sizeof(err));
    hourglass(0);
    S.searched = 1;
    if (S.nfound < 0) {
        S.nfound = 0;
        snprintf(S.si_status, sizeof(S.si_status), "Can't look: %s", err);
    } else if (!S.nfound) {
        snprintf(S.si_status, sizeof(S.si_status), "Type its address below.");
    } else {
        snprintf(S.si_status, sizeof(S.si_status), "Found %d.", S.nfound);
    }
    if (S.field >= si_fields())
        S.field = 0;
    if (S.browser_open && S.page == PG_SIGNIN)
        force_redraw(S.browser_w, 0, -0x7FFFFFF, S.scr_w, 0);
}

/* A DLNA server (a description's address, or a host and port) made the
   one in use, then its home page */
void dl_use(const char *addr)
{
    plex_ctx c = S.px;
    int e;
    hourglass(1);
    e = dlna_connect(&c, addr);
    hourglass(0);
    if (e != 0) {
        si_set_status("%s", c.err);
        return;
    }
    server_changed();
    server_take(&c);
    dl_remember();
    choices_save();
    signin_close();
    browser_top();
    set_status("%s: a DLNA server.", S.px.server_name);
}

/* DLNA server k of the saved ones */
int use_dl(int k)
{
    plex_ctx c = S.px;
    if (k < 0 || k >= S.ndl)
        return -1;
    c.kind = SRV_DLNA;
    c.local = 1;
    snprintf(c.base, sizeof(c.base), "%s", S.dl[k].loc);
    snprintf(c.server_name, sizeof(c.server_name), "%s", S.dl[k].name);
    snprintf(c.server_id, sizeof(c.server_id), "%s", S.dl[k].udn);
    c.token[0] = c.user_id[0] = c.user_name[0] = 0;
    c.ctl[0] = c.svc[0] = 0;        /* its description read again (the address may have changed) */
    server_changed();
    server_take(&c);
    choices_save();
    browser_top();
    return 0;
}

/* The DLNA page of the sign-in, and a look for servers */
void dl_add(void)
{
    if (!*S.dl_addr && S.px.kind == SRV_DLNA) {
        const char *h = strstr(S.px.base, "://");
        h = h ? h + 3 : S.px.base;
        snprintf(S.dl_addr, sizeof(S.dl_addr), "%.*s", (int)strcspn(h, "/"), h);
    }
    S.nfound = 0;
    S.searched = 0;
    signin_show(SI_DLNA);
    dl_search();
}

void sign_out(void)
{
    plex_ctx *c = &S.px;
    builtin_stop(0);
    if (S.libs.n || S.libs.v)
        plex_list_free(&S.libs);    /* no tabs until signed in again */
    if (c->kind == SRV_JELLYFIN) {
        /* that Jellyfin server forgotten (its token is no use now); the
           Plex account and the other servers kept */
        hourglass(1);
        jf_logout(c);
        hourglass(0);
        for (int i = 0; i < S.njf; i++)
            if (!strcmp(S.jf[i].id, c->server_id) && !strcmp(S.jf[i].uid, c->user_id)) {
                memmove(&S.jf[i], &S.jf[i + 1], (S.njf - i - 1) * sizeof(jf_saved));
                S.njf--;
                break;
            }
        c->base[0] = c->token[0] = c->server_name[0] = c->server_id[0] = 0;
        c->user_id[0] = c->user_name[0] = 0;
        c->kind = SRV_PLEX;
    } else if (c->kind == SRV_DLNA) {
        /* that DLNA server forgotten (where you got to in its videos is kept,
           for if it's added again) */
        for (int i = 0; i < S.ndl; i++)
            if (!strcmp(S.dl[i].udn, c->server_id)) {
                memmove(&S.dl[i], &S.dl[i + 1], (S.ndl - i - 1) * sizeof(dl_saved));
                S.ndl--;
                break;
            }
        c->base[0] = c->server_name[0] = c->server_id[0] = c->ctl[0] = c->svc[0] = 0;
        c->kind = SRV_PLEX;
    } else {
        c->account_token[0] = c->base[0] = c->token[0] = c->server_name[0] = c->server_id[0] = 0;
        S.nservers = 0;
        S.nusers = -1;
        S.user_uuid[0] = S.user_title[0] = 0;
    }
    if (S.browser_open)
        close_window(S.browser_w);
    S.browser_open = 0;
    list_forget();
    S.nhist = 0;
    S.page = PG_GRID;
    if (S.have_det)
        plex_list_free(&S.det);
    S.have_det = 0;
    cache_free_all();
    choices_save();
}

/* Forget a server (from Forget a server: FG_*), asked first. The one in
   use goes as Sign out does it; another is dropped from the list (a
   Jellyfin one signed out of on the server, if it answers). */
void forget_server(int what)
{
    char q[300], name[80];
    int k = what >= FG_DL(0) ? what - FG_DL(0) : what >= FG_JF(0) ? what - FG_JF(0) : 0;
    int in_use;
    if (what == FG_PLEX) {
        in_use = S.px.kind == SRV_PLEX;
        snprintf(q, sizeof(q), "Forget Plex? Matinee signs out of your Plex account and forgets its servers; "
                 "you'll need a new code from plex.tv/link (or a server's token) to add it again.");
    } else if (what >= FG_DL(0)) {
        if (k >= S.ndl)
            return;
        in_use = S.px.kind == SRV_DLNA && !strcmp(S.dl[k].udn, S.px.server_id);
        latin1(S.dl[k].name, name, sizeof(name));
        snprintf(q, sizeof(q), "Forget %s? Matinee forgets this DLNA server; add it again with Add a server. "
                 "(Where you got to in its videos is kept.)", name);
    } else {
        if (k >= S.njf)
            return;
        in_use = S.px.kind == SRV_JELLYFIN && !strcmp(S.jf[k].id, S.px.server_id) && !strcmp(S.jf[k].uid, S.px.user_id);
        latin1(S.jf[k].name, name, sizeof(name));
        snprintf(q, sizeof(q), "Forget %s? Matinee signs out of this Jellyfin server and forgets it; you'll need "
                 "to sign in to it again (Add a server).", name);
    }
    if (!ask(q))
        return;                     /* not confirmed: nothing forgotten */
    if (in_use) {
        sign_out();
        return;
    }
    if (what == FG_PLEX) {          /* the server in use is another kind: it stays */
        S.px.account_token[0] = 0;
        S.nservers = 0;
        S.nusers = -1;
        S.user_uuid[0] = S.user_title[0] = 0;
        set_status("Signed out of Plex.");
    } else if (what >= FG_DL(0)) {
        snprintf(name, sizeof(name), "%s", S.dl[k].name);
        memmove(&S.dl[k], &S.dl[k + 1], (S.ndl - k - 1) * sizeof(dl_saved));
        S.ndl--;
        set_status("Forgot %s.", name);
    } else {
        plex_ctx c = S.px;          /* its token is no use after: signed out on the server too */
        c.kind = SRV_JELLYFIN;
        snprintf(c.base, sizeof(c.base), "%s", S.jf[k].base);
        snprintf(c.token, sizeof(c.token), "%s", S.jf[k].token);
        hourglass(1);
        jf_logout(&c);
        hourglass(0);
        snprintf(name, sizeof(name), "%s", S.jf[k].name);
        memmove(&S.jf[k], &S.jf[k + 1], (S.njf - k - 1) * sizeof(jf_saved));
        S.njf--;
        set_status("Forgot %s.", name);
    }
    choices_save();
}

/* Select on the icon bar icon */
void bar_select(void)
{
    if (*S.px.base) {
        if (S.have_list)
            browser_open();
        else
            browser_top();
    } else if (*S.px.account_token) {
        choose_server();
    } else if (S.njf) {
        use_jf(S.njf - 1);
    } else if (S.ndl) {
        use_dl(S.ndl - 1);
    } else {
        signin_open();
    }
}
