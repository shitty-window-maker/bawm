/* bawm — barely a window manager.
   Single virtual screen. Mirror physical outputs via xrandr in autostart.
   build: cc -O2 tinywm.c -o bawm -lX11 -lyaml */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrandr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <yaml.h>

static int  is_userfloat(Window w);
static void set_userfloat(Window w, int on);

/* forward declarations */
static void raise_floats(void);
static void tile_apply(void);

/* ---------------------------------------------------------------- */
#define WS_MAX    9
#define BIND_MAX  128
#define RULE_MAX  64
#define AUTO_MAX  32
#define ERR_MAX   32

enum {
    A_NONE=0, A_SPAWN, A_CLOSE, A_CYCLE, A_CYCLE_PREV, A_QUIT, A_RELOAD,
    A_WORKSPACE, A_MOVE_WS,
    A_FOCUS_L, A_FOCUS_R, A_FOCUS_U, A_FOCUS_D,
    A_SWAP_L,  A_SWAP_R,  A_SWAP_U,  A_SWAP_D,
    A_SPLIT_FLIP, A_FULLSCREEN, A_FLOAT, A_TILE_TOGGLE,
    A_GAPS_TOGGLE, A_GAPS_INC, A_GAPS_DEC, A_RAISE, A_FLOAT_ALL, A_WINDOWED,
};

typedef struct { char *key; unsigned int mod; KeyCode code; int act; char *arg; } Bind;
typedef struct { char *cls, *title; int floating, workspace, skip; } Rule;
typedef struct Tile {
    Window win;      /* 0 if this leaf is empty (ghost or dead) */
    Window ghost;    /* nonzero: window that was here, moved away */
    struct Tile *a, *b;
    int dir;         /* 0=vertical 1=horizontal */
    double ratio;
} Tile;

static Display *dpy;
static int      scr;
static Window   root, focused = None, wmcheck = None;
static int      SW, SH;                /* virtual screen size (single mon) */
static int      cur_ws = 1, checking = 0, in_switch = 0;
static Time     last_time = 0;

static int      bw = 1;
static unsigned long col_focus = 0xc97064, col_unfocus = 0x444444;
static int      gap_in = 4, gap_out = 8, tile_default = 1;
static int      gap_off = 0, save_in = 4, save_out = 8;
static int      focus_follows = 0;
static int      global_float   = 0;

static Bind     binds[BIND_MAX]; static int n_binds;
static Rule     rules[RULE_MAX]; static int n_rules;
static char    *autostart[AUTO_MAX]; static int n_auto;
static Tile    *tree[WS_MAX + 1] = {0};
static int      tiled[WS_MAX + 1] = {0};

static unsigned int mv_mod = Mod4Mask, mv_btn = Button1;
static unsigned int rz_mod = Mod4Mask, rz_btn = Button3;
static unsigned int fc_mod = 0,        fc_btn = Button1;

static Atom A_UTF8, A_SUPP, A_CHECK, A_CLIENTS, A_ACTIVE, A_CLOSEW, A_DESKTOP,
            A_USERFLOAT,
            A_NDESK, A_WMDESK, A_WMNAME, A_WMTYPE, A_STATE, A_FS,
            A_WORKAREA, A_WMSTATE, A_TAKEFOCUS, A_PROTOS,
            A_WS, A_TITLE, A_PANEL_H, A_FSGEOM, A_PANEL_GEOM,
            A_WT_DIALOG, A_WT_UTIL, A_WT_TOOLBAR, A_WT_SPLASH, A_WT_MENU,
            A_WT_DROPDOWN, A_WT_POPUP, A_WT_TOOLTIP, A_WT_NOTIF,
            A_WT_DOCK, A_WT_DESKTOP;

static char     cfg_path[1024];
static int      ipc_fd = -1;
static char    *errs[ERR_MAX]; static int n_err;

static void cerr(const char *fmt, ...) {
    if (n_err >= ERR_MAX) return;
    char b[256]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    errs[n_err++] = strdup(b);
}
static void errs_clear(void) {
    for (int i = 0; i < n_err; i++) free(errs[i]);
    n_err = 0;
}

/* ---------------------------------------------------------------- */
/* helpers */
/* ---------------------------------------------------------------- */

static long get_card(Window w, Atom a, long def) {
    Atom at; int af; unsigned long ni, ba; unsigned char *d = NULL;
    long r = def;
    if (XGetWindowProperty(dpy, w, a, 0, 1, False, AnyPropertyType,
                           &at,&af,&ni,&ba,&d) == Success) {
        if (d && ni >= 1) r = ((long*)d)[0];
        if (d) XFree(d);
    }
    return r;
}
static void set_card(Window w, Atom a, long v) {
    XChangeProperty(dpy, w, a, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char*)&v, 1);
}
static int has_prop(Window w, Atom a) {
    Atom at; int af; unsigned long ni, ba; unsigned char *d = NULL;
    int r = 0;
    if (XGetWindowProperty(dpy, w, a, 0, 0, False, AnyPropertyType,
                           &at,&af,&ni,&ba,&d) == Success) {
        r = (at != None); if (d) XFree(d);
    }
    return r;
}
static int get_ws(Window w, int def) {
    long v = get_card(w, A_WMDESK, -1);
    return (v < 0) ? def : (int)v + 1;
}
static void set_ws(Window w, int ws) {
    set_card(w, A_WS, ws);
    set_card(w, A_WMDESK, ws - 1);
}

static Window top_level(Window w) {
    Window rr, pr, *kids; unsigned int n;
    while (w != None && w != root) {
        if (!XQueryTree(dpy, w, &rr, &pr, &kids, &n)) break;
        if (kids) XFree(kids);
        if (pr == root || pr == None) return w;
        w = pr;
    }
    return w;
}


/* A window the WM should not manage: desktop background, docks, etc.
   These get mapped and left alone — no border, no tile, no focus. */
static int is_desktop_window(Window w) {
    /* override-redirect windows belong to nobody's client list:
       panel, dropdowns, drag preview, notifications overlays. */
    XWindowAttributes wa;
    if (XGetWindowAttributes(dpy, w, &wa) && wa.override_redirect) return 1;

    Atom at; int af; unsigned long ni, ba; unsigned char *d = NULL;
    if (XGetWindowProperty(dpy, w, A_WMTYPE, 0, 16, False, XA_ATOM,
                           &at, &af, &ni, &ba, &d) == Success && d) {
        Atom *a = (Atom*)d;
        for (unsigned long i = 0; i < ni; i++)
            if (a[i] == A_WT_DESKTOP) { XFree(d); return 1; }
        XFree(d);
    }
    return 0;
}

static int is_float(Window w) {
    if (is_userfloat(w)) return 1;
    Window t = None;
    if (XGetTransientForHint(dpy, w, &t) && t != None && t != root) return 1;
    Atom at; int af; unsigned long ni, ba; unsigned char *d = NULL;
    if (XGetWindowProperty(dpy, w, A_WMTYPE, 0, 16, False, XA_ATOM,
                           &at,&af,&ni,&ba,&d) == Success) {
        if (d) {
            Atom *a = (Atom*)d;
            for (unsigned long i = 0; i < ni; i++) {
                if (a[i]==A_WT_DIALOG||a[i]==A_WT_UTIL||a[i]==A_WT_TOOLBAR||
                    a[i]==A_WT_SPLASH||a[i]==A_WT_MENU||a[i]==A_WT_DROPDOWN||
                    a[i]==A_WT_POPUP||a[i]==A_WT_TOOLTIP||a[i]==A_WT_NOTIF||
                    a[i]==A_WT_DOCK||a[i]==A_WT_DESKTOP) {
                    XFree(d); return 1;
                }
            }
            XFree(d);
        }
    }
    XSizeHints sh; long sup;
    if (XGetWMNormalHints(dpy, w, &sh, &sup)) {
        if ((sh.flags & PMinSize) && (sh.flags & PMaxSize) &&
            sh.min_width == sh.max_width && sh.min_height == sh.max_height)
            return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* atoms + EWMH publish */
/* ---------------------------------------------------------------- */

static void init_atoms(void) {
    A_UTF8       = XInternAtom(dpy, "UTF8_STRING", False);
    A_USERFLOAT  = XInternAtom(dpy, "_BAWM_USERFLOAT", False);
    A_SUPP       = XInternAtom(dpy, "_NET_SUPPORTED", False);
    A_CHECK      = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    A_CLIENTS    = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    A_ACTIVE     = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    A_CLOSEW     = XInternAtom(dpy, "_NET_CLOSE_WINDOW", False);
    A_DESKTOP    = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    A_NDESK      = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    A_WMDESK     = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    A_WMNAME     = XInternAtom(dpy, "_NET_WM_NAME", False);
    A_WMTYPE     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    A_STATE      = XInternAtom(dpy, "_NET_WM_STATE", False);
    A_FS         = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    A_WORKAREA   = XInternAtom(dpy, "_NET_WORKAREA", False);
    A_WMSTATE    = XInternAtom(dpy, "WM_STATE", False);
    A_TAKEFOCUS  = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    A_PROTOS     = XInternAtom(dpy, "WM_PROTOCOLS", False);
    A_WS         = XInternAtom(dpy, "_BAWM_WS", False);
    A_TITLE      = XInternAtom(dpy, "_BAWM_TITLE", False);
    A_PANEL_H    = XInternAtom(dpy, "_BAWM_PANEL_HEIGHT", False);
    A_FSGEOM     = XInternAtom(dpy, "_BAWM_FS_GEOM", False);
    A_PANEL_GEOM = XInternAtom(dpy, "_BAWM_PANEL_GEOM", False);
    A_WT_DIALOG   = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    A_WT_UTIL     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    A_WT_TOOLBAR  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    A_WT_SPLASH   = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    A_WT_MENU     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
    A_WT_DROPDOWN = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
    A_WT_POPUP    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    A_WT_TOOLTIP  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    A_WT_NOTIF    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    A_WT_DOCK     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    A_WT_DESKTOP  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
}

static void publish_clients(void) {
    Window rr, pr, *kids; unsigned int n;
    Window list[256]; int k = 0;
    if (XQueryTree(dpy, root, &rr, &pr, &kids, &n)) {
        for (unsigned int i = 0; i < n && k < 256; i++) {
            XWindowAttributes wa;
            if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
            if (wa.override_redirect) continue;
            if (is_desktop_window(kids[i])) continue;
            list[k++] = kids[i];
        }
        if (kids) XFree(kids);
    }
    XChangeProperty(dpy, root, A_CLIENTS, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char*)list, k);
}
static void publish_active(void) {
    Window w = focused ? focused : None;
    XChangeProperty(dpy, root, A_ACTIVE, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char*)&w, 1);
    XFlush(dpy);
}
static void publish_ws(void) {
    set_card(root, A_WS, cur_ws);
    set_card(root, A_DESKTOP, cur_ws - 1);
    XFlush(dpy);
}
static void publish_workarea(void) {
    int ph = (int)get_card(root, A_PANEL_H, 0);
    long a[4] = { 0, ph, SW, SH - ph };
    long data[WS_MAX * 4];
    for (int d = 0; d < WS_MAX; d++)
        for (int k = 0; k < 4; k++) data[d*4+k] = a[k];
    XChangeProperty(dpy, root, A_WORKAREA, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char*)data, WS_MAX * 4);
    XFlush(dpy);
}
static void publish_panel_geom(void) {
    char b[64];
    snprintf(b, sizeof b, "0 0 %d %d", SW, SH);
    XChangeProperty(dpy, root, A_PANEL_GEOM, XA_STRING, 8, PropModeReplace,
                    (unsigned char*)b, strlen(b));
}
static void publish_title(void) {
    char *t = NULL;
    if (focused) {
        Atom at; int af; unsigned long ni, ba; unsigned char *d = NULL;
        if (XGetWindowProperty(dpy, focused, A_WMNAME, 0, 512, False, A_UTF8,
                               &at,&af,&ni,&ba,&d) == Success && d && ni)
            t = strndup((char*)d, ni);
        if (d) XFree(d);
        if (!t) { char *f = NULL; if (XFetchName(dpy, focused, &f) && f) {
            t = strdup(f); XFree(f); } }
    }
    if (!t) t = strdup("");
    XChangeProperty(dpy, root, A_TITLE, A_UTF8, 8, PropModeReplace,
                    (unsigned char*)t, strlen(t));
    free(t);
    XFlush(dpy);
}
static void setup_ewmh(void) {
    wmcheck = XCreateSimpleWindow(dpy, root, -100, -100, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, wmcheck, A_CHECK, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char*)&wmcheck, 1);
    XChangeProperty(dpy, root, A_CHECK, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char*)&wmcheck, 1);
    const char *nm = "bawm";
    XChangeProperty(dpy, wmcheck, A_WMNAME, A_UTF8, 8, PropModeReplace,
                    (unsigned char*)nm, strlen(nm));
    Atom sup[] = { A_SUPP, A_CLIENTS, A_ACTIVE, A_CLOSEW, A_DESKTOP,
                   A_NDESK, A_WMDESK, A_STATE, A_FS, A_WORKAREA };
    XChangeProperty(dpy, root, A_SUPP, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)sup, sizeof(sup)/sizeof(Atom));
    set_card(root, A_NDESK, WS_MAX);
}

/* ---------------------------------------------------------------- */
/* tile tree */
/* ---------------------------------------------------------------- */

static Tile *leaf(Window w) {
    Tile *n = calloc(1, sizeof *n);
    n->win = w; n->ghost = 0; n->ratio = 0.5;
    return n;
}
static Tile *split(Tile *a, Tile *b, int dir) {
    Tile *n = calloc(1, sizeof *n); n->a = a; n->b = b;
    n->dir = dir; n->ratio = 0.5; return n;
}
static Tile *tfind(Tile *r, Window w) {
    if (!r) return NULL;
    if (r->win == w) return r;
    Tile *f = tfind(r->a, w); return f ? f : tfind(r->b, w);
}
static Tile **tslot(Tile **r, Window w) {
    if (!*r) return NULL;
    if ((*r)->win == w) return r;
    Tile **f = tslot(&(*r)->a, w); return f ? f : tslot(&(*r)->b, w);
}
static void tfree(Tile *r) {
    if (!r) return;
    tfree(r->a);
    tfree(r->b);
    free(r);
}
static void tremove(Tile **r, Window w) {
    if (!*r) return;
    if ((*r)->win || (*r)->ghost) {
        /* leaf: match either an actual window or a ghost of it */
        if ((*r)->win == w || (*r)->ghost == w) { free(*r); *r = NULL; }
        return;
    }
    tremove(&(*r)->a, w); tremove(&(*r)->b, w);
    if (!(*r)->a && !(*r)->b) { free(*r); *r = NULL; return; }
    if (!(*r)->a) { Tile *b = (*r)->b; free(*r); *r = b; return; }
    if (!(*r)->b) { Tile *a = (*r)->a; free(*r); *r = a; return; }
}

/* Turn a live leaf into a ghost: keeps its slot in the tree, but the
   window id moves out. Used when a window is sent to another workspace. */
static void tghost(Tile **r, Window w) {
    if (!*r) return;
    if ((*r)->win == w) { (*r)->ghost = w; (*r)->win = 0; return; }
    if ((*r)->win || (*r)->ghost) return;
    tghost(&(*r)->a, w); tghost(&(*r)->b, w);
}
/* If a ghost slot exists for w on this workspace, revive it in place. */
static int revive_ghost(Tile **r, Window w) {
    if (!*r) return 0;
    if ((*r)->ghost == w) { (*r)->win = w; (*r)->ghost = 0; return 1; }
    if ((*r)->win || (*r)->ghost) return 0;
    return revive_ghost(&(*r)->a, w) || revive_ghost(&(*r)->b, w);
}
/* Deep-copy a Tile subtree so we can restore proportions on reinsert. */

static int count_leaves(Tile *r) {
    if (!r) return 0;
    if (r->win) return 1;
    if (r->ghost && !r->a && !r->b) return 0;
    return count_leaves(r->a) + count_leaves(r->b);
}
static void tinsert(int ws, Window target, Window nw) {
    if (!tree[ws]) { tree[ws] = leaf(nw); return; }
    Tile **slot = target ? tslot(&tree[ws], target) : NULL;
    if (!slot) slot = &tree[ws];
    Tile *old = *slot;
    int n = count_leaves(tree[ws]);
    *slot = split(old, leaf(nw), n & 1);
}
static void tlayout(Tile *r, int x, int y, int w, int h) {
    if (!r) return;
    if (r->win) {
        XMoveResizeWindow(dpy, r->win,
            x + bw + gap_in/2, y + bw + gap_in/2,
            w - 2*bw - gap_in, h - 2*bw - gap_in);
        return;
    }
    if (r->ghost && !r->a && !r->b) {
        /* ghost leaf: keep the slot reserved, no window to move */
        return;
    }
    if (r->dir == 0) {
        int aw = (int)(w * r->ratio);
        tlayout(r->a, x, y, aw, h);
        tlayout(r->b, x + aw, y, w - aw, h);
    } else {
        int ah = (int)(h * r->ratio);
        tlayout(r->a, x, y, w, ah);
        tlayout(r->b, x, y + ah, w, h - ah);
    }
}
static int tile_apply_ph(void) { return (int)get_card(root, A_PANEL_H, 0); }

/* Ask RandR for the bounding box of all active CRTCs. When two outputs
   are mirrored (same origin, same size), this returns the shared size,
   not the pre-mirror extended size. Called from tile_apply so the
   layout is always correct even if we miss an RRScreenChangeNotify. */
static void read_screen_size(void) {
    int nw = 0, nh = 0;
    XRRScreenResources *res = XRRGetScreenResources(dpy, root);
    if (res) {
        for (int i = 0; i < res->ncrtc; i++) {
            XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, res->crtcs[i]);
            if (!ci) continue;
            if (ci->mode != None && ci->width && ci->height) {
                int rx = (int)ci->x + (int)ci->width;
                int ry = (int)ci->y + (int)ci->height;
                if (rx > nw) nw = rx;
                if (ry > nh) nh = ry;
            }
            XRRFreeCrtcInfo(ci);
        }
        XRRFreeScreenResources(res);
    }
    if (nw <= 0 || nh <= 0) {
        nw = DisplayWidth(dpy, scr);
        nh = DisplayHeight(dpy, scr);
    }
    if (nw != SW || nh != SH) {
        fprintf(stderr, "bawm: screen now %dx%d\n", nw, nh);
        SW = nw; SH = nh;
        publish_panel_geom();
        publish_workarea();
    }
}

static void tile_apply(void) {
    read_screen_size();
    if (cur_ws < 1 || cur_ws > WS_MAX || !tiled[cur_ws] || !tree[cur_ws]) {
        XFlush(dpy); return;
    }
    int ph = tile_apply_ph();
    tlayout(tree[cur_ws], 0, ph, SW, SH - ph);
    XFlush(dpy);
    raise_floats();
}

/* ---------------------------------------------------------------- */
/* focus */
/* ---------------------------------------------------------------- */

static void send_take_focus(Window w, Time t) {
    Atom *p = NULL; int n = 0;
    if (!XGetWMProtocols(dpy, w, &p, &n)) return;
    for (int i = 0; i < n; i++) {
        if (p[i] == A_TAKEFOCUS) {
            XEvent e = {0};
            e.xclient.type = ClientMessage;
            e.xclient.window = w;
            e.xclient.message_type = A_PROTOS;
            e.xclient.format = 32;
            e.xclient.data.l[0] = A_TAKEFOCUS;
            e.xclient.data.l[1] = t ? t : CurrentTime;
            XSendEvent(dpy, w, False, NoEventMask, &e);
            break;
        }
    }
    XFree(p);
}
static void focus(Window w, Time t) {
    if (!t) t = last_time ? last_time : CurrentTime;
    if (w && is_desktop_window(w)) return;
    if (w && !has_prop(w, A_WMDESK)) return;
    if (w == focused) {
        /* even if already focused, keep floats above tiles */
        publish_title();
        return;
    }
    if (focused) XSetWindowBorder(dpy, focused, col_unfocus);
    focused = w;
    if (w) {
        XSetWindowBorder(dpy, w, col_focus);
        send_take_focus(w, t);
        XSetInputFocus(dpy, w, RevertToPointerRoot, t);
        /* floats always raise; tiled only raise if the user opted in */
        if (is_float(w) ||
            cur_ws < 1 || cur_ws > WS_MAX || !tiled[cur_ws])
            XRaiseWindow(dpy, w);
    }
    publish_title();
    publish_active();
    raise_floats();
}
static void close_win(Window w) {
    if (!w) return;
    /* refuse to close what the WM doesn't manage:
       panel, docks, popups, anything the WM never assigned a workspace */
    if (is_desktop_window(w)) return;
    if (!has_prop(w, A_WMDESK)) return;
    Atom wp = XInternAtom(dpy, "WM_PROTOCOLS", False);
    Atom wd = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XEvent e = {0};
    e.xclient.type = ClientMessage; e.xclient.window = w;
    e.xclient.message_type = wp; e.xclient.format = 32;
    e.xclient.data.l[0] = wd; e.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, w, False, NoEventMask, &e);
    fprintf(stderr, "bawm: close 0x%lx\n", (unsigned long)w);
}

/* ---------------------------------------------------------------- */
/* config */
/* ---------------------------------------------------------------- */

static int parse_mod(const char *p, unsigned int *m) {
    if (!strcasecmp(p,"M")||!strcasecmp(p,"Alt")||!strcasecmp(p,"Mod1")) *m |= Mod1Mask;
    else if (!strcasecmp(p,"S")||!strcasecmp(p,"Shift")) *m |= ShiftMask;
    else if (!strcasecmp(p,"C")||!strcasecmp(p,"Ctrl")||!strcasecmp(p,"Control")) *m |= ControlMask;
    else if (!strcasecmp(p,"Super")||!strcasecmp(p,"Mod4")||!strcasecmp(p,"Win")) *m |= Mod4Mask;
    else return 0;
    return 1;
}
static int split_plus(const char *v, char **parts, int *nout) {
    static char bufs[4][256]; static int idx = 0;
    char *buf = bufs[idx = (idx + 1) % 4];
    strncpy(buf, v, 255); buf[255] = 0;
    int n = 0; char *sv = NULL;
    for (char *tk = strtok_r(buf, "+", &sv); tk && n < 16;
         tk = strtok_r(NULL, "+", &sv)) {
        while (*tk == ' ' || *tk == '"') tk++;
        char *e = tk + strlen(tk) - 1;
        while (e > tk && (*e == ' ' || *e == '"')) *e-- = 0;
        parts[n++] = tk;
    }
    *nout = n; return n > 0;
}
static int parse_key(const char *v, char **key, unsigned int *mod) {
    char *p[16]; int n;
    if (!split_plus(v, p, &n)) return 0;
    *key = strdup(p[n-1]); *mod = 0;
    for (int i = 0; i < n-1; i++) parse_mod(p[i], mod);
    return 1;
}
static int parse_mouse(const char *v, unsigned int *mod, unsigned int *btn) {
    char *p[8]; int n;
    if (!split_plus(v, p, &n)) return 0;
    const char *b = p[n-1];
    unsigned int bu = 0;
    if      (!strcasecmp(b,"Button1")||!strcasecmp(b,"B1")) bu = Button1;
    else if (!strcasecmp(b,"Button2")||!strcasecmp(b,"B2")) bu = Button2;
    else if (!strcasecmp(b,"Button3")||!strcasecmp(b,"B3")) bu = Button3;
    else return 0;
    unsigned int m = 0;
    for (int i = 0; i < n-1; i++) parse_mod(p[i], &m);
    *mod = m; *btn = bu; return 1;
}
static int parse_act(const char *s) {
    if (!s) return A_NONE;
    #define M(k,v) if (!strcmp(s,k)) return v;
    M("spawn",A_SPAWN) M("close",A_CLOSE) M("cycle",A_CYCLE)
    M("cycle_prev",A_CYCLE_PREV) M("quit",A_QUIT) M("reload",A_RELOAD)
    M("workspace",A_WORKSPACE) M("move_to_workspace",A_MOVE_WS)
    M("focus_left",A_FOCUS_L)  M("focus_right",A_FOCUS_R)
    M("focus_up",A_FOCUS_U)    M("focus_down",A_FOCUS_D)
    M("swap_left",A_SWAP_L)    M("swap_right",A_SWAP_R)
    M("swap_up",A_SWAP_U)      M("swap_down",A_SWAP_D)
    M("focus_dir",0) /* placeholder; handled below */
    M("swap_dir",0)
    M("toggle_split",A_SPLIT_FLIP) M("fullscreen",A_FULLSCREEN)
    M("float_toggle",A_FLOAT) M("tile_toggle",A_TILE_TOGGLE)
    M("gaps_toggle",A_GAPS_TOGGLE) M("gaps_inc",A_GAPS_INC)
    M("gaps_dec",A_GAPS_DEC) M("raise",A_RAISE)
    M("windowed",A_WINDOWED)
    M("float_all",A_FLOAT_ALL)
    #undef M
    /* direction args */
    return A_NONE;
}
/* special-case focus_dir / swap_dir since they carry an arg */
static int parse_act_dir(const char *act, const char *arg, int *out) {
    if (!act || !arg) return 0;
    if (!strcmp(act,"focus_dir")) {
        if (*arg=='l') *out = A_FOCUS_L;
        else if (*arg=='r') *out = A_FOCUS_R;
        else if (*arg=='u') *out = A_FOCUS_U;
        else if (*arg=='d') *out = A_FOCUS_D;
        else return 0;
        return 1;
    }
    if (!strcmp(act,"swap_dir")) {
        if (*arg=='l') *out = A_SWAP_L;
        else if (*arg=='r') *out = A_SWAP_R;
        else if (*arg=='u') *out = A_SWAP_U;
        else if (*arg=='d') *out = A_SWAP_D;
        else return 0;
        return 1;
    }
    return 0;
}
static void add_bind_full(const char *k, int act, const char *arg) {
    if (n_binds >= BIND_MAX) { cerr("too many binds"); return; }
    Bind *b = &binds[n_binds];
    if (!parse_key(k, &b->key, &b->mod)) { cerr("bad key '%s'", k); return; }
    b->code = 0; b->act = act; b->arg = arg ? strdup(arg) : NULL;
    n_binds++;
}
static void add_bind(const char *k, const char *act, const char *arg) {
    int a = parse_act(act);
    if (a == A_NONE) {
        if (!parse_act_dir(act, arg, &a)) {
            cerr("unknown action '%s'", act);
            return;
        }
    }
    add_bind_full(k, a, arg);
}
static void clear_binds(void) {
    for (int i = 0; i < n_binds; i++) {
        if (dpy && binds[i].code) {
            XUngrabKey(dpy, binds[i].code, binds[i].mod, root);
            XUngrabKey(dpy, binds[i].code, binds[i].mod|LockMask, root);
            XUngrabKey(dpy, binds[i].code, binds[i].mod|Mod2Mask, root);
            XUngrabKey(dpy, binds[i].code, binds[i].mod|LockMask|Mod2Mask, root);
        }
        free(binds[i].key); free(binds[i].arg);
    }
    n_binds = 0;
}
static void set_defaults(void) {
    bw = 1; col_focus = 0xc97064; col_unfocus = 0x444444;
    gap_in = 4; gap_out = 8; tile_default = 1;
    mv_mod = Mod4Mask; mv_btn = Button1;
    rz_mod = Mod4Mask; rz_btn = Button3;
    clear_binds();
    add_bind("Super+Return",     "spawn", "kitty");
    add_bind("Super+d",          "spawn", "dmenu_run");
    add_bind("Super+Shift+c",    "close", NULL);
    add_bind("Super+Shift+q",    "quit", NULL);
    add_bind("Super+Shift+r",    "reload", NULL);
    add_bind("Super+Tab",        "cycle", NULL);
    add_bind("Super+Shift+Tab",  "cycle_prev", NULL);
    add_bind("Super+t",          "tile_toggle", NULL);
    add_bind("Super+Shift+t",    "float_all",   NULL);
    add_bind("Super+h",  "focus_dir", "l");
    add_bind("Super+j",  "focus_dir", "d");
    add_bind("Super+k",  "focus_dir", "u");
    add_bind("Super+l",  "focus_dir", "r");
    add_bind("Super+Shift+h", "swap_dir", "l");
    add_bind("Super+Shift+j", "swap_dir", "d");
    add_bind("Super+Shift+k", "swap_dir", "u");
    add_bind("Super+Shift+l", "swap_dir", "r");
    add_bind("Super+s",        "toggle_split", NULL);
    add_bind("Super+r",        "raise", NULL);
    add_bind("Super+f",        "float_toggle", NULL);
    add_bind("Super+Shift+f",  "windowed",   NULL);
    add_bind("Super+F11",      "fullscreen", NULL);
    add_bind("Super+g",        "gaps_toggle", NULL);
    add_bind("Super+equal",    "gaps_inc", NULL);
    add_bind("Super+minus",    "gaps_dec", NULL);
    for (int i = 1; i <= WS_MAX; i++) {
        char k[32], a[8];
        snprintf(k, sizeof k, "Super+%d", i);
        snprintf(a, sizeof a, "%d", i);
        add_bind(k, "workspace", a);
        snprintf(k, sizeof k, "Super+Shift+%d", i);
        add_bind(k, "move_to_workspace", a);
    }
}
static unsigned long parse_color(const char *s) {
    while (*s == ' ' || *s == '"') s++;
    if (*s == '#') s++;
    return strtoul(s, NULL, 16) & 0xffffff;
}
static void set_section(const char *sec, const char *k, const char *v) {
    if (!strcmp(sec, "border")) {
        if (!strcmp(k,"width")) bw = atoi(v);
        else if (!strcmp(k,"focused")) col_focus = parse_color(v);
        else if (!strcmp(k,"unfocused")) col_unfocus = parse_color(v);
    } else if (!strcmp(sec, "tile")) {
        if (!strcmp(k,"gaps_in")) gap_in = atoi(v);
        else if (!strcmp(k,"gaps_out")) gap_out = atoi(v);
        else if (!strcmp(k,"gap")) { gap_in = gap_out = atoi(v); }
        else if (!strcmp(k,"default_on"))
            tile_default = (!strcasecmp(v,"true") || atoi(v));
    } else if (!strcmp(sec, "mouse")) {
        if (!strcmp(k,"move"))   parse_mouse(v, &mv_mod, &mv_btn);
        else if (!strcmp(k,"resize")) parse_mouse(v, &rz_mod, &rz_btn);
        else if (!strcmp(k,"focus"))  parse_mouse(v, &fc_mod, &fc_btn);
        else if (!strcmp(k,"focus_follows"))
            focus_follows = (!strcasecmp(v,"true") || atoi(v));
    }
}
static void parse_keys(yaml_document_t *d, yaml_node_t *seq) {
    clear_binds();
    for (yaml_node_item_t *it = seq->data.sequence.items.start;
         it < seq->data.sequence.items.top; it++) {
        yaml_node_t *item = yaml_document_get_node(d, *it);
        if (!item || item->type != YAML_MAPPING_NODE) continue;
        char *k = NULL, *a = NULL, *arg = NULL;
        for (yaml_node_pair_t *p = item->data.mapping.pairs.start;
             p < item->data.mapping.pairs.top; p++) {
            yaml_node_t *kk = yaml_document_get_node(d, p->key);
            yaml_node_t *vv = yaml_document_get_node(d, p->value);
            if (!kk || !vv || kk->type != YAML_SCALAR_NODE ||
                vv->type != YAML_SCALAR_NODE) continue;
            const char *KS = (char*)kk->data.scalar.value;
            const char *VS = (char*)vv->data.scalar.value;
            if      (!strcmp(KS,"key")) k = (char*)VS;
            else if (!strcmp(KS,"action")) a = (char*)VS;
            else if (!strcmp(KS,"arg")) arg = (char*)VS;
        }
        if (k && a) add_bind(k, a, arg);
        else cerr("line %d: key entry missing 'key' or 'action'",
                  item->start_mark.line + 1);
    }
}
static void parse_rules(yaml_document_t *d, yaml_node_t *seq) {
    for (int i = 0; i < n_rules; i++) {
        free(rules[i].cls); free(rules[i].title);
    }
    n_rules = 0;
    for (yaml_node_item_t *it = seq->data.sequence.items.start;
         it < seq->data.sequence.items.top && n_rules < RULE_MAX; it++) {
        yaml_node_t *item = yaml_document_get_node(d, *it);
        if (!item || item->type != YAML_MAPPING_NODE) continue;
        char *c = NULL, *t = NULL; int fl = -1, ws = 0, sk = -1;
        for (yaml_node_pair_t *p = item->data.mapping.pairs.start;
             p < item->data.mapping.pairs.top; p++) {
            yaml_node_t *kk = yaml_document_get_node(d, p->key);
            yaml_node_t *vv = yaml_document_get_node(d, p->value);
            if (!kk || !vv || kk->type != YAML_SCALAR_NODE ||
                vv->type != YAML_SCALAR_NODE) continue;
            const char *KS = (char*)kk->data.scalar.value;
            const char *VS = (char*)vv->data.scalar.value;
            if      (!strcmp(KS,"class")) c = (char*)VS;
            else if (!strcmp(KS,"title")) t = (char*)VS;
            else if (!strcmp(KS,"float")) fl = (!strcasecmp(VS,"true")||atoi(VS));
            else if (!strcmp(KS,"workspace")) ws = atoi(VS);
            else if (!strcmp(KS,"skip")) sk = (!strcasecmp(VS,"true")||atoi(VS));
        }
        if (!c && !t) continue;
        Rule *r = &rules[n_rules++];
        r->cls = c ? strdup(c) : NULL;
        r->title = t ? strdup(t) : NULL;
        r->floating = fl; r->workspace = ws; r->skip = sk;
    }
}
static void parse_autostart(yaml_document_t *d, yaml_node_t *seq) {
    for (int i = 0; i < n_auto; i++) { free(autostart[i]); autostart[i] = NULL; }
    n_auto = 0;
    for (yaml_node_item_t *it = seq->data.sequence.items.start;
         it < seq->data.sequence.items.top && n_auto < AUTO_MAX; it++) {
        yaml_node_t *item = yaml_document_get_node(d, *it);
        if (!item || item->type != YAML_SCALAR_NODE) continue;
        const char *c = (char*)item->data.scalar.value;
        if (c && *c) autostart[n_auto++] = strdup(c);
    }
    fprintf(stderr, "bawm: %d autostart command(s)\n", n_auto);
}
static void run_autostart(void) {
    for (int i = 0; i < n_auto; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            if (ipc_fd >= 0) close(ipc_fd);
            execlp("/bin/sh","sh","-c",autostart[i],NULL);
            _exit(1);
        }
    }
}
static void parse_config(const char *path) {
    errs_clear();
    FILE *f = fopen(path, "r");
    if (!f) { cerr("no config at %s", path); return; }
    yaml_parser_t p; yaml_document_t d;
    yaml_parser_initialize(&p);
    yaml_parser_set_input_file(&p, f);
    if (!yaml_parser_load(&p, &d)) {
        cerr("YAML error line %d: %s", p.problem_mark.line + 1,
             p.problem ? p.problem : "?");
        yaml_parser_delete(&p); fclose(f); return;
    }
    yaml_node_t *r = yaml_document_get_root_node(&d);
    if (r && r->type == YAML_MAPPING_NODE) {
        for (yaml_node_pair_t *pp = r->data.mapping.pairs.start;
             pp < r->data.mapping.pairs.top; pp++) {
            yaml_node_t *k = yaml_document_get_node(&d, pp->key);
            yaml_node_t *v = yaml_document_get_node(&d, pp->value);
            if (!k || k->type != YAML_SCALAR_NODE) continue;
            const char *ks = (char*)k->data.scalar.value;
            if (v->type == YAML_MAPPING_NODE) {
                for (yaml_node_pair_t *p2 = v->data.mapping.pairs.start;
                     p2 < v->data.mapping.pairs.top; p2++) {
                    yaml_node_t *k2 = yaml_document_get_node(&d, p2->key);
                    yaml_node_t *v2 = yaml_document_get_node(&d, p2->value);
                    if (!k2 || !v2 || k2->type != YAML_SCALAR_NODE ||
                        v2->type != YAML_SCALAR_NODE) continue;
                    set_section(ks, (char*)k2->data.scalar.value,
                                (char*)v2->data.scalar.value);
                }
            } else if (v->type == YAML_SEQUENCE_NODE) {
                if      (!strcmp(ks,"keys"))      parse_keys(&d, v);
                else if (!strcmp(ks,"rules"))     parse_rules(&d, v);
                else if (!strcmp(ks,"autostart")) parse_autostart(&d, v);
            }
        }
    }
    yaml_document_delete(&d);
    yaml_parser_delete(&p);
    fclose(f);
}
static void write_default(const char *path) {
    char dir[1024];
    strncpy(dir, path, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdir(dir, 0755); }
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(
"# bawm config\n"
"#   reload:   Super+Shift+r\n"
"#   validate: bawm --check\n"
"\n"
"border:\n"
"  width: 1\n"
"  focused:   \"#c97064\"\n"
"  unfocused: \"#444444\"\n"
"\n"
"tile:\n"
"  gaps_in:  4\n"
"  gaps_out: 8\n"
"  default_on: true\n"
"\n"
"mouse:\n"
"  move:   \"Super+Button1\"\n"
"  resize: \"Super+Button3\"\n"
"  focus_follows: true\n"
"\n"
"rules:\n"
"  - { class: \"Pavucontrol\",     float: true }\n"
"  - { class: \"Nm-connection\",   float: true }\n"
"  - { class: \"Lxappearance\",    float: true }\n"
"  - { class: \"Gcolor3\",         float: true }\n"
"  - { class: \"Blueman-manager\", float: true }\n"
"  - { class: \"Galculator\",      float: true }\n"
"  - { class: \"Arandr\",          float: true }\n"
"  - { class: \"Mpv\",             float: true }\n"
"  - { class: \"mpv\",             float: true }\n"
"  - { class: \"vlc\",             float: true }\n"
"  - { class: \"feh\",             float: true }\n"
"  - { class: \"Firefox\",         float: false }\n"
"  - { class: \"discord\",         workspace: 3 }\n"
"  - { class: \"Spotify\",         workspace: 4 }\n"
"  - { class: \"Steam\",           workspace: 5 }\n"
"  - { class: \"steam\",           workspace: 5 }\n"
"\n"
"autostart:\n"
"  # - \"xrandr --output DP-0   --mode 1920x1080 --rate 60 --pos 0x0 --primary\"\n"
"  # - \"xrandr --output HDMI-0 --mode 1920x1080 --rate 60 --same-as DP-0\"\n"
"  - \"feh --no-fehbg --bg-solid #101010\"\n"
"  # - \"picom --backend glx --vsync\"\n"
"  # - \"dunst\"\n"
"  - \"$HOME/Documents/tinywm/bawm-panel\"\n"
"  - \"$HOME/.config/bawm/status.sh\"\n"
"  - \"xset r rate 250 40\"\n"
"  - \"xset b off\"\n"
"\n"
"keys:\n"
"  - { key: \"Super+Return\",       action: spawn, arg: \"kitty\" }\n"
"  - { key: \"Super+d\",            action: spawn, arg: \"dmenu_run\" }\n"
"  - { key: \"Super+Shift+Return\", action: spawn, arg: \"kitty -e tmux\" }\n"
"  - { key: \"Super+Shift+d\",      action: spawn, arg: \"rofi -show drun\" }\n"
"  - { key: \"Super+Shift+c\", action: close }\n"
"  - { key: \"Super+Shift+q\", action: quit }\n"
"  - { key: \"Super+Shift+r\", action: reload }\n"
"  - { key: \"Super+r\",       action: raise }\n"
"  - { key: \"Super+Tab\",       action: cycle }\n"
"  - { key: \"Super+Shift+Tab\", action: cycle_prev }\n"
"  - { key: \"Super+t\",       action: tile_toggle }\n"
"  - { key: \"Super+Shift+t\", action: float_all }\n"
"  - { key: \"Super+h\", action: focus_dir, arg: \"l\" }\n"
"  - { key: \"Super+j\", action: focus_dir, arg: \"d\" }\n"
"  - { key: \"Super+k\", action: focus_dir, arg: \"u\" }\n"
"  - { key: \"Super+l\", action: focus_dir, arg: \"r\" }\n"
"  - { key: \"Super+Shift+h\", action: swap_dir, arg: \"l\" }\n"
"  - { key: \"Super+Shift+j\", action: swap_dir, arg: \"d\" }\n"
"  - { key: \"Super+Shift+k\", action: swap_dir, arg: \"u\" }\n"
"  - { key: \"Super+Shift+l\", action: swap_dir, arg: \"r\" }\n"
"  - { key: \"Super+s\", action: toggle_split }\n"
"  - { key: \"Super+f\",       action: float_toggle }\n"
"  - { key: \"Super+Shift+f\", action: windowed }\n"
"  - { key: \"Super+F11\",     action: fullscreen }\n"
"  - { key: \"Super+g\",     action: gaps_toggle }\n"
"  - { key: \"Super+equal\", action: gaps_inc }\n"
"  - { key: \"Super+minus\", action: gaps_dec }\n"
"  - { key: \"Super+1\", action: workspace, arg: \"1\" }\n"
"  - { key: \"Super+2\", action: workspace, arg: \"2\" }\n"
"  - { key: \"Super+3\", action: workspace, arg: \"3\" }\n"
"  - { key: \"Super+4\", action: workspace, arg: \"4\" }\n"
"  - { key: \"Super+5\", action: workspace, arg: \"5\" }\n"
"  - { key: \"Super+6\", action: workspace, arg: \"6\" }\n"
"  - { key: \"Super+7\", action: workspace, arg: \"7\" }\n"
"  - { key: \"Super+8\", action: workspace, arg: \"8\" }\n"
"  - { key: \"Super+9\", action: workspace, arg: \"9\" }\n"
"  - { key: \"Super+Shift+1\", action: move_to_workspace, arg: \"1\" }\n"
"  - { key: \"Super+Shift+2\", action: move_to_workspace, arg: \"2\" }\n"
"  - { key: \"Super+Shift+3\", action: move_to_workspace, arg: \"3\" }\n"
"  - { key: \"Super+Shift+4\", action: move_to_workspace, arg: \"4\" }\n"
"  - { key: \"Super+Shift+5\", action: move_to_workspace, arg: \"5\" }\n"
"  - { key: \"Super+Shift+6\", action: move_to_workspace, arg: \"6\" }\n"
"  - { key: \"Super+Shift+7\", action: move_to_workspace, arg: \"7\" }\n"
"  - { key: \"Super+Shift+8\", action: move_to_workspace, arg: \"8\" }\n"
"  - { key: \"Super+Shift+9\", action: move_to_workspace, arg: \"9\" }\n", f);
    fclose(f);
    fprintf(stderr, "bawm: wrote default config to %s\n", path);
}

/* ---------------------------------------------------------------- */
/* actions */
/* ---------------------------------------------------------------- */

static void apply_rules(Window w, int *ws, int *flt, int *skip_out) {
    *ws = 0; *flt = -1; if (skip_out) *skip_out = -1;
    if (!n_rules) return;
    char *cls = NULL, *inst = NULL, *title = NULL;
    XClassHint ch = {0};
    if (XGetClassHint(dpy, w, &ch)) { cls = ch.res_class; inst = ch.res_name; }
    Atom at; int af; unsigned long ni, ba; unsigned char *d = NULL;
    if (XGetWindowProperty(dpy, w, A_WMNAME, 0, 256, False, A_UTF8,
                           &at,&af,&ni,&ba,&d) == Success && d && ni)
        title = strndup((char*)d, ni);
    if (d) XFree(d);
    if (!title) { char *f = NULL; if (XFetchName(dpy, w, &f) && f) {
        title = strdup(f); XFree(f); } }
    for (int i = 0; i < n_rules; i++) {
        Rule *r = &rules[i];
        if (r->cls) {
            int m = 0;
            if (cls && strcasestr(cls, r->cls)) m = 1;
            if (!m && inst && strcasestr(inst, r->cls)) m = 1;
            if (!m) continue;
        }
        if (r->title && (!title || !strcasestr(title, r->title))) continue;
        if (r->floating >= 0) *flt = r->floating;
        if (r->workspace) *ws = r->workspace;
        if (r->skip >= 0 && skip_out) *skip_out = r->skip;
    }
    if (ch.res_name) XFree(ch.res_name);
    if (ch.res_class) XFree(ch.res_class);
    free(title);
}

static void ws_show(int ws) {
    Window rr, pr, *kids; unsigned int n;
    if (!XQueryTree(dpy, root, &rr, &pr, &kids, &n)) return;
    for (unsigned int i = 0; i < n; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
        if (wa.override_redirect) continue;
        /* if the WM never assigned a workspace to this window,
           it's not ours to hide (panel, dock, desktop) */
        if (!has_prop(kids[i], A_WMDESK)) continue;
        int kws = get_ws(kids[i], 0);
        int vis = (kws == ws);
        if (vis && wa.map_state != IsViewable) XMapWindow(dpy, kids[i]);
        else if (!vis && wa.map_state == IsViewable) {
            set_card(kids[i], A_WMSTATE, IconicState);
            XUnmapWindow(dpy, kids[i]);
        }
    }
    if (kids) XFree(kids);
}
static void do_switch_ws(int n) {
    if (n < 1 || n > WS_MAX || n == cur_ws) return;
    cur_ws = n;
    in_switch = 1;
    ws_show(n);
    in_switch = 0;
    focused = None;
    publish_ws();
    tile_apply();
    XSync(dpy, False);
    raise_floats();
}
static void do_move_to_ws(int n) {
    if (n < 1 || n > WS_MAX || !focused) return;
    Window w = focused;
    int from = cur_ws;
    if (tiled[from]) { tghost(&tree[from], w); tile_apply(); }
    set_ws(w, n);
    if (n != cur_ws) {
        focused = None;
        set_card(w, A_WMSTATE, IconicState);
        XUnmapWindow(dpy, w);
        if (tiled[n]) tinsert(n, 0, w);
    } else {
        if (tiled[n]) tinsert(n, 0, w);
        tile_apply();
    }
    XSync(dpy, False);
}
static void do_cycle(int dir) {
    Window rr, pr, *kids; unsigned int n;
    if (!XQueryTree(dpy, root, &rr, &pr, &kids, &n)) return;
    Window list[256]; int k = 0;
    for (unsigned int i = 0; i < n && k < 256; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
        if (wa.override_redirect || wa.map_state != IsViewable) continue;
        if (get_ws(kids[i], cur_ws) != cur_ws) continue;
        if (is_desktop_window(kids[i])) continue;
        list[k++] = kids[i];
    }
    if (kids) XFree(kids);
    if (!k) return;
    int idx = -1;
    for (int i = 0; i < k; i++) if (list[i] == focused) { idx = i; break; }
    idx = (idx < 0) ? 0 : (idx + dir + k) % k;
    focus(list[idx], last_time);
}
static void do_fullscreen(Window w, int on) {
    if (!w) return;
    if (on) {
        if (has_prop(w, A_FSGEOM)) return;
        XWindowAttributes wa;
        if (XGetWindowAttributes(dpy, w, &wa)) {
            long g[4] = { wa.x, wa.y, wa.width, wa.height };
            XChangeProperty(dpy, w, A_FSGEOM, XA_CARDINAL, 32, PropModeReplace,
                            (unsigned char*)g, 4);
        }
        if (cur_ws >= 1 && cur_ws <= WS_MAX && tiled[cur_ws] &&
            tfind(tree[cur_ws], w)) {
            tremove(&tree[cur_ws], w); tile_apply();
        }
        XMoveResizeWindow(dpy, w, 0, 0, SW, SH);
        XRaiseWindow(dpy, w);
        Atom two[1] = { A_FS };
        XChangeProperty(dpy, w, A_STATE, XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)two, 1);
    } else {
        Atom at; int af; unsigned long ni, ba; unsigned char *d = NULL;
        if (XGetWindowProperty(dpy, w, A_FSGEOM, 0, 4, True, AnyPropertyType,
                               &at,&af,&ni,&ba,&d) == Success) {
            if (d && ni == 4) {
                long *g = (long*)d;
                XMoveResizeWindow(dpy, w, g[0], g[1], g[2], g[3]);
            }
            if (d) XFree(d);
        }
        if (cur_ws >= 1 && cur_ws <= WS_MAX && tiled[cur_ws] &&
            !is_float(w) && !tfind(tree[cur_ws], w)) {
            tinsert(cur_ws, 0, w); tile_apply();
        }
        XDeleteProperty(dpy, w, A_STATE);
    }
}

static int is_userfloat(Window w) { return has_prop(w, A_USERFLOAT); }
static void set_userfloat(Window w, int on) {
    if (on) set_card(w, A_USERFLOAT, 1);
    else    XDeleteProperty(dpy, w, A_USERFLOAT);
}

static void do_float(Window w) {
    if (!w) return;
    w = top_level(w);
    if (!w || w == root) return;

    if (is_userfloat(w)) {
        /* put it back into the tiling */
        set_userfloat(w, 0);
        if (cur_ws >= 1 && cur_ws <= WS_MAX && tiled[cur_ws] &&
            !tfind(tree[cur_ws], w)) {
            tinsert(cur_ws, 0, w);
            tile_apply();
        }
        fprintf(stderr, "bawm: float OFF 0x%lx\n", (unsigned long)w);
    } else {
        set_userfloat(w, 1);
        if (cur_ws >= 1 && cur_ws <= WS_MAX && tiled[cur_ws]) {
            tremove(&tree[cur_ws], w);
            tile_apply();
        }
        int ph = tile_apply_ph();
        int ww = SW * 2 / 3;
        int wh = (SH - ph) * 2 / 3;
        XMoveResizeWindow(dpy, w, (SW - ww) / 2,
                          ph + (SH - ph - wh) / 2, ww, wh);
        XRaiseWindow(dpy, w);
        focus(w, last_time);
        fprintf(stderr, "bawm: float ON 0x%lx\n", (unsigned long)w);
    }
    XFlush(dpy);
}
static void do_split_flip(void) {
    if (!focused) return;
    if (cur_ws < 1 || cur_ws > WS_MAX || !tiled[cur_ws]) return;
    Tile *n = tfind(tree[cur_ws], focused);
    if (!n) return;
    Tile *stack[128]; int sp = 0; stack[sp++] = tree[cur_ws];
    Tile *parent = NULL;
    while (sp) {
        Tile *c = stack[--sp];
        if (!c || c->win) continue;
        if (c->a == n || c->b == n) { parent = c; break; }
        if (c->a) stack[sp++] = c->a;
        if (c->b) stack[sp++] = c->b;
    }
    if (!parent) return;
    parent->dir = !parent->dir;
    tile_apply();
}
static void do_gaps_toggle(void) {
    if (gap_off) { gap_in = save_in; gap_out = save_out; gap_off = 0; }
    else { save_in = gap_in; save_out = gap_out; gap_in = gap_out = 0; gap_off = 1; }
    tile_apply();
}
static void do_dir(int side, int swap) {
    if (!focused) return;
    if (cur_ws < 1 || cur_ws > WS_MAX || !tree[cur_ws]) return;
    Tile *leaves[128]; int lx[128], ly[128], lw[128], lh[128]; int n = 0;
    Tile *stk[128]; int sx[128], sy[128], sw[128], sh[128]; int sp = 0;
    int ph = tile_apply_ph();
    stk[sp]=tree[cur_ws]; sx[sp]=0; sy[sp]=ph; sw[sp]=SW; sh[sp]=SH-ph; sp++;
    while (sp && n < 128) {
        sp--;
        Tile *c = stk[sp]; int X=sx[sp], Y=sy[sp], W=sw[sp], H=sh[sp];
        if (!c) continue;
        if (c->win) { leaves[n]=c; lx[n]=X; ly[n]=Y; lw[n]=W; lh[n]=H; n++; continue; }
        if (c->ghost && !c->a && !c->b) continue;   /* skip ghosts */
        if (c->dir == 0) {
            int aw = (int)(W * c->ratio);
            if (sp < 126) {
                stk[sp]=c->b; sx[sp]=X+aw; sy[sp]=Y; sw[sp]=W-aw; sh[sp]=H; sp++;
                stk[sp]=c->a; sx[sp]=X;    sy[sp]=Y; sw[sp]=aw;   sh[sp]=H; sp++;
            }
        } else {
            int ah = (int)(H * c->ratio);
            if (sp < 126) {
                stk[sp]=c->b; sx[sp]=X; sy[sp]=Y+ah; sw[sp]=W; sh[sp]=H-ah; sp++;
                stk[sp]=c->a; sx[sp]=X; sy[sp]=Y;    sw[sp]=W; sh[sp]=ah;   sp++;
            }
        }
    }
    int fi = -1;
    for (int i = 0; i < n; i++) if (leaves[i]->win == focused) { fi = i; break; }
    if (fi < 0) return;
    int fcx = lx[fi] + lw[fi] / 2, fcy = ly[fi] + lh[fi] / 2;
    int best = -1, best_d = 1 << 30;
    for (int i = 0; i < n; i++) {
        if (i == fi) continue;
        int cx = lx[i] + lw[i] / 2, cy = ly[i] + lh[i] / 2;
        int dx = cx - fcx, dy = cy - fcy;
        int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        int ok = 0;
        if (side=='l') ok = cx < fcx && ady < (lh[fi]+lh[i])/2;
        else if (side=='r') ok = cx > fcx && ady < (lh[fi]+lh[i])/2;
        else if (side=='u') ok = cy < fcy && adx < (lw[fi]+lw[i])/2;
        else ok = cy > fcy && adx < (lw[fi]+lw[i])/2;
        if (!ok) continue;
        int d = dx*dx + dy*dy;
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best < 0) return;
    if (!swap) focus(leaves[best]->win, last_time);
    else {
        Window a = leaves[fi]->win, b = leaves[best]->win;
        leaves[fi]->win = b; leaves[best]->win = a;
        tile_apply();
    }
}
static void do_tile_toggle(void) {
    if (cur_ws < 1 || cur_ws > WS_MAX) return;
    tiled[cur_ws] = !tiled[cur_ws];
    if (tiled[cur_ws]) {
        tfree(tree[cur_ws]); tree[cur_ws] = NULL;
        Window rr, pr, *kids; unsigned int n;
        if (XQueryTree(dpy, root, &rr, &pr, &kids, &n)) {
            for (unsigned int i = 0; i < n; i++) {
                XWindowAttributes wa;
                if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
                if (wa.override_redirect || wa.map_state != IsViewable) continue;
                if (get_ws(kids[i], cur_ws) != cur_ws) continue;
                if (is_float(kids[i])) continue;
                tinsert(cur_ws, 0, kids[i]);
            }
            if (kids) XFree(kids);
        }
    }
    tile_apply();
}
static void do_spawn(const char *cmd) {
    if (!cmd || !*cmd) return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (dpy) close(ConnectionNumber(dpy));
        if (ipc_fd >= 0) close(ipc_fd);
        execlp("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
    }
}

/* floats always stack above tiles. Called after any focus/raise so
   tiles never end up covering a floating window. */
static void raise_floats(void) {
    Window rr, pr, *kids; unsigned int n;
    if (!XQueryTree(dpy, root, &rr, &pr, &kids, &n)) return;
    for (unsigned int i = 0; i < n; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
        if (wa.override_redirect) continue;
        if (is_desktop_window(kids[i])) continue;
        if (wa.map_state != IsViewable) continue;
        if (get_ws(kids[i], cur_ws) != cur_ws) continue;
        if (is_float(kids[i])) XRaiseWindow(dpy, kids[i]);
    }
    if (kids) XFree(kids);
    /* focused float stays at the very top of the pile */
    if (focused && is_float(focused)) XRaiseWindow(dpy, focused);
}

/* Global float mode: every workspace stops tiling, every window floats.
   Turning it off re-tiles the current workspace from the current viewable
   windows and restores tile_default for the rest. */
static void do_float_all(void) {
    int ph = tile_apply_ph();
    Window rr, pr, *kids; unsigned int n;

    if (global_float) {
        /* back to tiling */
        global_float = 0;
        for (int ws = 1; ws <= WS_MAX; ws++) tiled[ws] = tile_default;
        if (cur_ws >= 1 && cur_ws <= WS_MAX && tiled[cur_ws]) {
            tfree(tree[cur_ws]); tree[cur_ws] = NULL;
            if (XQueryTree(dpy, root, &rr, &pr, &kids, &n)) {
                for (unsigned int i = 0; i < n; i++) {
                    XWindowAttributes wa;
                    if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
                    if (wa.override_redirect || wa.map_state != IsViewable) continue;
                    if (get_ws(kids[i], cur_ws) != cur_ws) continue;
                    if (is_float(kids[i])) continue;
                    tinsert(cur_ws, 0, kids[i]);
                }
                if (kids) XFree(kids);
            }
            tile_apply();
        }
        fprintf(stderr, "bawm: float mode OFF\n");
    } else {
        /* every workspace floats */
        global_float = 1;
        for (int ws = 1; ws <= WS_MAX; ws++) {
            tiled[ws] = 0;
            tfree(tree[ws]); tree[ws] = NULL;
        }
        /* cascade current-workspace windows so they don't stack on 0,0 */
        if (XQueryTree(dpy, root, &rr, &pr, &kids, &n)) {
            int idx = 0;
            for (unsigned int i = 0; i < n; i++) {
                XWindowAttributes wa;
                if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
                if (wa.override_redirect || wa.map_state != IsViewable) continue;
                if (get_ws(kids[i], cur_ws) != cur_ws) continue;
                int ww = SW * 2 / 3;
                int wh = (SH - ph) * 2 / 3;
                int x = 40 + (idx % 8) * 40;
                int y = ph + 40 + (idx % 8) * 40;
                if (x + ww > SW) x = SW - ww - 10;
                if (y + wh > SH) y = SH - wh - 10;
                XMoveResizeWindow(dpy, kids[i], x, y, ww, wh);
                XRaiseWindow(dpy, kids[i]);
                idx++;
            }
            if (kids) XFree(kids);
        }
        fprintf(stderr, "bawm: float mode ON\n");
    }
    XFlush(dpy);
}


/* A "windowed" float: distinct from the tile->float toggle. Smaller,
   centered, sized like a normal desktop app. Useful for "give me a
   proper window I can move around" without changing the tiling layout. */
static void do_windowed(Window w) {
    if (!w) return;
    w = top_level(w);
    if (!w || w == root) return;
    set_userfloat(w, 1);
    if (cur_ws >= 1 && cur_ws <= WS_MAX && tiled[cur_ws]) {
        tremove(&tree[cur_ws], w);
        tile_apply();
    }
    int ph = tile_apply_ph();
    int ww = SW * 5 / 8;
    int wh = (SH - ph) * 5 / 8;
    if (ww < 480) ww = 480;
    if (wh < 360) wh = 360;
    if (ww > SW - 40) ww = SW - 40;
    if (wh > SH - ph - 40) wh = SH - ph - 40;
    XMoveResizeWindow(dpy, w, (SW - ww) / 2,
                      ph + (SH - ph - wh) / 2, ww, wh);
    XRaiseWindow(dpy, w);
    focus(w, last_time);
    fprintf(stderr, "bawm: windowed 0x%lx\n", (unsigned long)w);
    XFlush(dpy);
}

static void do_action(int a, char *arg) {
    switch (a) {
    case A_SPAWN:        do_spawn(arg); break;
    case A_CLOSE:        close_win(focused); break;
    case A_CYCLE:        do_cycle(1); break;
    case A_CYCLE_PREV:   do_cycle(-1); break;
    case A_QUIT:         exit(0);
    case A_RELOAD:       break;
    case A_WORKSPACE:    if (arg) do_switch_ws(atoi(arg)); break;
    case A_MOVE_WS:      if (arg) do_move_to_ws(atoi(arg)); break;
    case A_FOCUS_L:      do_dir('l', 0); break;
    case A_FOCUS_R:      do_dir('r', 0); break;
    case A_FOCUS_U:      do_dir('u', 0); break;
    case A_FOCUS_D:      do_dir('d', 0); break;
    case A_SWAP_L:       do_dir('l', 1); break;
    case A_SWAP_R:       do_dir('r', 1); break;
    case A_SWAP_U:       do_dir('u', 1); break;
    case A_SWAP_D:       do_dir('d', 1); break;
    case A_SPLIT_FLIP:   do_split_flip(); break;
    case A_FULLSCREEN:   do_fullscreen(focused, !has_prop(focused, A_FSGEOM)); break;
    case A_FLOAT:        do_float(focused); break;
    case A_TILE_TOGGLE:  do_tile_toggle(); break;
    case A_GAPS_TOGGLE:  do_gaps_toggle(); break;
    case A_GAPS_INC:     gap_in = (gap_in + 2 > 64) ? 64 : gap_in + 2;
                         gap_off = 0; tile_apply(); break;
    case A_GAPS_DEC:     gap_in = (gap_in < 2) ? 0 : gap_in - 2;
                         tile_apply(); break;
    case A_WINDOWED:     do_windowed(focused); break;
    case A_FLOAT_ALL:    do_float_all(); break;
    case A_RAISE:        if (focused) XRaiseWindow(dpy, focused);
                         XFlush(dpy); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- */
/* drag */
/* ---------------------------------------------------------------- */


/* ---- drop preview overlay (hollow rect via XShape) ---- */
static Window pv_win = None;
static int    pv_has_shape = 0;

static void pv_init(void) {
    if (pv_win != None) return;
    XSetWindowAttributes a = {
        .override_redirect = True,
        .background_pixel  = 0xc97064,
        .event_mask        = 0,
    };
    pv_win = XCreateWindow(dpy, root, 0, 0, 10, 10, 0, CopyFromParent,
                           InputOutput, CopyFromParent,
                           CWOverrideRedirect|CWBackPixel|CWEventMask, &a);
    int ev, er;
    pv_has_shape = XShapeQueryExtension(dpy, &ev, &er);
}
static void pv_show(int x, int y, int w, int h) {
    if (w < 6 || h < 6) { if (pv_win) XUnmapWindow(dpy, pv_win); return; }
    pv_init();
    XMoveResizeWindow(dpy, pv_win, x, y, w, h);
    if (pv_has_shape) {
        int t = 4;
        XRectangle outer = { 0, 0, w, h };
        XRectangle inner = { t, t, w - t*2, h - t*2 };
        XShapeCombineRectangles(dpy, pv_win, ShapeBounding, 0, 0,
                                &outer, 1, ShapeSet, Unsorted);
        XShapeCombineRectangles(dpy, pv_win, ShapeBounding, 0, 0,
                                &inner, 1, ShapeSubtract, Unsorted);
    }
    XMapRaised(dpy, pv_win);
}
static void pv_hide(void) {
    if (pv_win != None) XUnmapWindow(dpy, pv_win);
}

/* Split the target leaf in the given direction with the dragged window.
   side: 0=L 1=R 2=T 3=B — dragged takes that half, target keeps the other. */
static void tile_split_leaf(int ws, Window dragged, Window target, int side) {
    Tile **rootp = &tree[ws];
    if (!*rootp) return;
    tremove(rootp, dragged);
    if (!*rootp) { *rootp = leaf(dragged); return; }
    Tile **slot = tslot(rootp, target);
    if (!slot) { *rootp = split(*rootp, leaf(dragged), 0); return; }
    int dir    = (side == 0 || side == 1) ? 0 : 1;
    int before = (side == 0 || side == 2);
    Tile *old = *slot;
    Tile *nw  = leaf(dragged);
    *slot = before ? split(nw, old, dir) : split(old, nw, dir);
    fprintf(stderr, "bawm: drop SPLIT %s\n",
            side == 0 ? "L" : side == 1 ? "R" : side == 2 ? "T" : "B");
}

static void drag_window(Window w, int resize) {
    w = top_level(w);
    if (!w || w == root) return;
    XRaiseWindow(dpy, w);
    Window rr, cr; int rx, ry, wx, wy; unsigned int m;
    XQueryPointer(dpy, root, &rr, &cr, &rx, &ry, &wx, &wy, &m);
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    if (XGrabPointer(dpy, root, False, PointerMotionMask|ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, None, None,
                     CurrentTime) != GrabSuccess) return;
    for (;;) {
        XEvent e; XNextEvent(dpy, &e);
        if (e.type == MotionNotify) {
            int dx = e.xmotion.x_root - rx, dy = e.xmotion.y_root - ry;
            if (resize) {
                int nw = wa.width + dx, nh = wa.height + dy;
                if (nw < 50) nw = 50;
                if (nh < 50) nh = 50;
                XResizeWindow(dpy, w, nw, nh);
            } else {
                XMoveWindow(dpy, w, wa.x + dx, wa.y + dy);
            }
        } else if (e.type == ButtonRelease) break;
    }
    XUngrabPointer(dpy, CurrentTime);
    XFlush(dpy);
}

static int tbounds(Tile *r, Tile *t, int x, int y, int w, int h,
                   int *ox, int *oy, int *ow, int *oh) {
    if (!r) return 0;
    if (r == t) { *ox = x; *oy = y; *ow = w; *oh = h; return 1; }
    if (r->win || (r->ghost && !r->a && !r->b)) return 0;
    if (r->dir == 0) {
        int aw = (int)(w * r->ratio);
        if (tbounds(r->a, t, x, y, aw, h, ox,oy,ow,oh)) return 1;
        return tbounds(r->b, t, x+aw, y, w-aw, h, ox,oy,ow,oh);
    } else {
        int ah = (int)(h * r->ratio);
        if (tbounds(r->a, t, x, y, w, ah, ox,oy,ow,oh)) return 1;
        return tbounds(r->b, t, x, y+ah, w, h-ah, ox,oy,ow,oh);
    }
}

static void tile_drag(Window w) {
    w = top_level(w);
    if (!w || w == root) return;
    if (cur_ws < 1 || cur_ws > WS_MAX || !tiled[cur_ws]) {
        drag_window(w, 0); return;
    }
    Tile *src = tfind(tree[cur_ws], w);
    if (!src) { drag_window(w, 0); return; }

    Window rr, cr; int rx, ry, wx, wy; unsigned int m;
    XQueryPointer(dpy, root, &rr, &cr, &rx, &ry, &wx, &wy, &m);
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    if (XGrabPointer(dpy, root, False,
                     PointerMotionMask|ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, None, None,
                     CurrentTime) != GrabSuccess) return;

    XRaiseWindow(dpy, w);
    XFlush(dpy);

    int ph = tile_apply_ph();
    Tile *last_hover = NULL;
    int last_side = -1;
    int dragging = 0;
    const int SLOP = 4;

    /* compute the dest rectangle for the current cursor position */
    #define HIT_TEST(MX, MY, DST, SIDE) do { \
        (DST) = NULL; (SIDE) = -1; \
        Tile *stk[128]; int sx[128], sy[128], sw[128], sh[128]; int sp = 0; \
        stk[sp]=tree[cur_ws]; sx[sp]=0; sy[sp]=ph; sw[sp]=SW; sh[sp]=SH-ph; sp++; \
        while (sp) { \
            sp--; \
            Tile *c = stk[sp]; int X=sx[sp], Y=sy[sp], W=sw[sp], H=sh[sp]; \
            if (!c) continue; \
            if (c->win) { \
                if ((MX)>=X && (MX)<X+W && (MY)>=Y && (MY)<Y+H) { \
                    (DST) = c; \
                    int cx0 = X + W/3, cy0 = Y + H/3; \
                    int cw = W/3, ch = H/3; \
                    if ((MX) >= cx0 && (MX) < cx0+cw && \
                        (MY) >= cy0 && (MY) < cy0+ch) { (SIDE) = 4; } \
                    else { \
                        int dl = (MX) - X, dr = X+W - (MX); \
                        int dt = (MY) - Y, db = Y+H - (MY); \
                        int bm = dl; int bs = 0; \
                        if (dr < bm) { bm = dr; bs = 1; } \
                        if (dt < bm) { bm = dt; bs = 2; } \
                        if (db < bm) { bm = db; bs = 3; } \
                        (SIDE) = bs; \
                    } \
                } \
                continue; \
            } \
            if (c->dir == 0) { \
                int aw = (int)(W * c->ratio); \
                if (sp < 126) { \
                    stk[sp]=c->b; sx[sp]=X+aw; sy[sp]=Y; sw[sp]=W-aw; sh[sp]=H; sp++; \
                    stk[sp]=c->a; sx[sp]=X;    sy[sp]=Y; sw[sp]=aw;   sh[sp]=H; sp++; \
                } \
            } else { \
                int ah = (int)(H * c->ratio); \
                if (sp < 126) { \
                    stk[sp]=c->b; sx[sp]=X; sy[sp]=Y+ah; sw[sp]=W; sh[sp]=H-ah; sp++; \
                    stk[sp]=c->a; sx[sp]=X; sy[sp]=Y;    sw[sp]=W; sh[sp]=ah;   sp++; \
                } \
            } \
        } \
    } while (0)

    for (;;) {
        XEvent e; XNextEvent(dpy, &e);
        if (e.type == MotionNotify) {
            int dx = e.xmotion.x_root - rx;
            int dy = e.xmotion.y_root - ry;
            if (!dragging && dx*dx + dy*dy > SLOP*SLOP) {
                dragging = 1;
            }
            if (dragging) {
                XMoveWindow(dpy, w, wa.x + dx, wa.y + dy);
                XFlush(dpy);

                Tile *hover; int side;
                HIT_TEST(e.xmotion.x_root, e.xmotion.y_root, hover, side);

                if (hover && hover != src && side >= 0) {
                    int hx, hy, hw, hh;
                    if (tbounds(tree[cur_ws], hover,
                                0, ph, SW, SH - ph, &hx, &hy, &hw, &hh)) {
                        int dx2 = hx, dy2 = hy, dw2 = hw, dh2 = hh;
                        if (side == 0)      dw2 = hw / 2;
                        else if (side == 1) { dx2 = hx + hw/2; dw2 = hw/2; }
                        else if (side == 2) dh2 = hh / 2;
                        else if (side == 3) { dy2 = hy + hh/2; dh2 = hh/2; }
                        /* side 4 = centre → full outline (means swap) */
                        if (hover != last_hover || side != last_side) {
                            pv_show(dx2, dy2, dw2, dh2);
                            last_hover = hover; last_side = side;
                        }
                    }
                } else {
                    if (last_hover) { pv_hide(); last_hover = NULL; last_side = -1; }
                }
                XRaiseWindow(dpy, w);
            }
        } else if (e.type == ButtonRelease) {
            pv_hide();
            if (!dragging) {
                /* pure click — replay to the app with the original time */
                XAllowEvents(dpy, ReplayPointer, e.xbutton.time);
            } else {
                Tile *dst; int side;
                HIT_TEST(e.xbutton.x_root, e.xbutton.y_root, dst, side);
                if (dst && dst != src && side >= 0) {
                    if (side == 4) {
                        Window tmp = dst->win;
                        dst->win = src->win;
                        src->win = tmp;
                        fprintf(stderr, "bawm: drop SWAP\n");
                        tile_apply();
                    } else {
                        tile_split_leaf(cur_ws, w, dst->win, side);
                        tile_apply();
                    }
                } else {
                    tile_apply();   /* outside — snap back */
                }
            }
            break;
        }
    }
    XUngrabPointer(dpy, CurrentTime);
    XSync(dpy, False);

    #undef HIT_TEST
}

/* ---------------------------------------------------------------- */
/* IPC */
/* ---------------------------------------------------------------- */

static void ipc_out(int fd, const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    write(fd, b, strlen(b));
}
static void ipc_cmd(int fd, char *line) {
    char *nl = strchr(line, '\n'); if (nl) *nl = 0;
    if (!*line) return;
    char *sv = NULL;
    char *c = strtok_r(line, " \t", &sv);
    if (!c) return;
    if (!strcmp(c,"workspace")) { char *a=strtok_r(NULL," \t",&sv);
        if (a) { do_switch_ws(atoi(a)); ipc_out(fd,"ok\n"); } else ipc_out(fd,"err\n"); }
    else if (!strcmp(c,"move_to_workspace")) { char *a=strtok_r(NULL," \t",&sv);
        if (a) { do_move_to_ws(atoi(a)); ipc_out(fd,"ok\n"); } else ipc_out(fd,"err\n"); }
    else if (!strcmp(c,"focus")||!strcmp(c,"swap")) {
        char *d = strtok_r(NULL," \t",&sv);
        if (d) { do_dir(d[0], !strcmp(c,"swap")); ipc_out(fd,"ok\n"); }
        else ipc_out(fd,"err\n");
    }
    else if (!strcmp(c,"spawn")) {
        while (*sv==' '||*sv=='\t') sv++;
        do_spawn(sv);
        ipc_out(fd,"ok\n");
    }
    else if (!strcmp(c,"fullscreen")) { do_fullscreen(focused, !has_prop(focused, A_FSGEOM)); ipc_out(fd,"ok\n"); }
    else if (!strcmp(c,"float_toggle")) { do_float(focused); ipc_out(fd,"ok\n"); }
    else if (!strcmp(c,"close")) { close_win(focused); ipc_out(fd,"ok\n"); }
    else if (!strcmp(c,"tile_toggle")) { do_tile_toggle(); ipc_out(fd,"ok\n"); }
    else if (!strcmp(c,"gaps_toggle")) { do_gaps_toggle(); ipc_out(fd,"ok\n"); }
    else if (!strcmp(c,"reload")) { ipc_out(fd,"reloading\n"); }
    else if (!strcmp(c,"quit")) { ipc_out(fd,"ok\n"); XSync(dpy,False); exit(0); }
    else if (!strcmp(c,"query")) {
        char *w = strtok_r(NULL," \t",&sv);
        if (!w) { ipc_out(fd,"err\n"); return; }
        if      (!strcmp(w,"workspace")) ipc_out(fd,"%d\n",cur_ws);
        else if (!strcmp(w,"title")) {
            Atom at; int af; unsigned long ni, ba; unsigned char *d = NULL;
            char *t = NULL;
            if (focused && XGetWindowProperty(dpy, focused, A_WMNAME, 0, 512,
                    False, A_UTF8, &at,&af,&ni,&ba,&d) == Success && d && ni) {
                t = strndup((char*)d, ni); XFree(d);
            }
            ipc_out(fd,"%s\n", t ? t : ""); free(t);
        }
        else ipc_out(fd,"err\n");
    }
    else ipc_out(fd,"unknown '%s'\n", c);
}
static void ipc_handle(void) {
    struct sockaddr_un ca; socklen_t cl = sizeof ca;
    int c = accept(ipc_fd, (struct sockaddr*)&ca, &cl);
    if (c < 0) return;
    struct timeval tv = { .tv_sec = 2 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char b[1024]; ssize_t n = read(c, b, sizeof b - 1);
    if (n > 0) { b[n] = 0; ipc_cmd(c, b); }
    close(c);
}
static void ipc_init(void) {
    const char *xdg = getenv("XDG_RUNTIME_DIR"), *home = getenv("HOME");
    char path[256];
    if (xdg && *xdg) snprintf(path, sizeof path, "%s/bawm.sock", xdg);
    else if (home && *home) snprintf(path, sizeof path, "%s/.bawm.sock", home);
    else snprintf(path, sizeof path, "/tmp/bawm-%d.sock", (int)getuid());
    unlink(path);
    ipc_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ipc_fd < 0) return;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (bind(ipc_fd, (struct sockaddr*)&a, sizeof a) < 0 ||
        listen(ipc_fd, 4) < 0) { close(ipc_fd); ipc_fd = -1; return; }
    fprintf(stderr, "bawm: ipc %s\n", path);
}

/* ---------------------------------------------------------------- */
/* key/button grabs */
/* ---------------------------------------------------------------- */

static void grab_key(KeyCode k, unsigned int m) {
    if (!k) return;
    XGrabKey(dpy, k, m, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, k, m|LockMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, k, m|Mod2Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, k, m|LockMask|Mod2Mask, root, True, GrabModeAsync, GrabModeAsync);
}
static void grab_btn(unsigned int b, unsigned int m) {
    if (!m) return;   /* never grab a plain button */
    XGrabButton(dpy, b, m, root, False, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, b, m|LockMask, root, False, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, b, m|Mod2Mask, root, False, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, b, m|LockMask|Mod2Mask, root, False, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);
}
static void grab_all(void) {
    for (int i = 0; i < n_binds; i++) {
        KeySym ks = XStringToKeysym(binds[i].key);
        if (ks == NoSymbol) { cerr("bad keysym %s", binds[i].key); continue; }
        binds[i].code = XKeysymToKeycode(dpy, ks);
        grab_key(binds[i].code, binds[i].mod);
    }
    if (mv_mod) grab_btn(mv_btn, mv_mod);
    if (rz_mod) grab_btn(rz_btn, rz_mod);
    if (fc_mod) grab_btn(fc_btn, fc_mod);   /* focus grab only if configured */
}
static void reload(void) {
    XUngrabButton(dpy, AnyButton, AnyModifier, root);
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    parse_config(cfg_path);
    grab_all();
    XSync(dpy, False);
    fprintf(stderr, "bawm: reloaded (%d binds, %d rules, %d errs)\n",
            n_binds, n_rules, n_err);
    for (int i = 0; i < n_err; i++) fprintf(stderr, "  err: %s\n", errs[i]);
}
static void handle_key(XKeyEvent *ev) {
    unsigned int st = ev->state & ~(LockMask | Mod2Mask);
    for (int i = 0; i < n_binds; i++) {
        if (binds[i].code != ev->keycode || binds[i].mod != st) continue;
        if (binds[i].act == A_RELOAD) { reload(); return; }
        do_action(binds[i].act, binds[i].arg);
        return;
    }
}

/* ---------------------------------------------------------------- */
/* main */
/* ---------------------------------------------------------------- */

static int xerror(Display *d, XErrorEvent *e) {
    (void)d;
    if (e->error_code == BadAccess && checking) {
        fprintf(stderr, "bawm: another wm is running\n");
        exit(1);
    }
    return 0;
}

int main(int argc, char **argv) {
    int check_only = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--check") || !strcmp(argv[i], "-c"))
            check_only = 1;

    signal(SIGCHLD, SIG_IGN);
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(cfg_path, sizeof cfg_path, "%s/bawm/config.yaml", xdg);
    else if (home && *home) snprintf(cfg_path, sizeof cfg_path,
                                     "%s/.config/bawm/config.yaml", home);
    else snprintf(cfg_path, sizeof cfg_path, "./bawm.yaml");

    set_defaults();
    FILE *t = fopen(cfg_path, "r");
    if (t) fclose(t);
    else write_default(cfg_path);
    parse_config(cfg_path);

    if (check_only) {
        printf("%d binds, %d rules, %d error(s)\n", n_binds, n_rules, n_err);
        for (int i = 0; i < n_err; i++) printf("  %s\n", errs[i]);
        return n_err ? 1 : 0;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "bawm: cannot open display\n"); return 1; }
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    SW = DisplayWidth(dpy, scr);
    SH = DisplayHeight(dpy, scr);
    XSetErrorHandler(xerror);

    init_atoms();
    setup_ewmh();

    checking = 1;
    XSelectInput(dpy, root,
                 SubstructureRedirectMask|SubstructureNotifyMask|
                 KeyPressMask|ButtonPressMask|PropertyChangeMask|
                 PointerMotionMask|EnterWindowMask|LeaveWindowMask);
    int rr_evt, rr_err;
    if (XRRQueryExtension(dpy, &rr_evt, &rr_err))
        XRRSelectInput(dpy, root, RRScreenChangeNotifyMask);
    XSync(dpy, False);
    checking = 0;

    grab_all();
    if (tile_default) for (int i = 1; i <= WS_MAX; i++) tiled[i] = 1;

    /* adopt any existing windows */
    {
        Window rr, pr, *kids; unsigned int n;
        if (XQueryTree(dpy, root, &rr, &pr, &kids, &n)) {
            for (unsigned int i = 0; i < n; i++) {
                XWindowAttributes wa;
                if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
                if (wa.override_redirect) continue;
                if (is_desktop_window(kids[i])) {
                    XSetWindowBorderWidth(dpy, kids[i], 0);
                    continue;
                }
                XSetWindowBorderWidth(dpy, kids[i], bw);
                XSetWindowBorder(dpy, kids[i], col_unfocus);
                XSelectInput(dpy, kids[i], PropertyChangeMask |
                                          StructureNotifyMask |
                                          EnterWindowMask |
                                          FocusChangeMask);
                set_ws(kids[i], cur_ws);
                if (wa.map_state == IsViewable) {
                    set_card(kids[i], A_WMSTATE, NormalState);
                    focus(kids[i], 0);
                    if (tiled[cur_ws] && !is_float(kids[i]))
                        tinsert(cur_ws, 0, kids[i]);
                }
            }
            if (kids) XFree(kids);
        }
    }

    ipc_init();

    fprintf(stderr,
            "\n  bawm — barely a window manager\n"
            "  --------------------------------\n"
            "  config: %s\n"
            "  screen: %dx%d\n"
            "  reload: Super+Shift+r   quit: Super+Shift+q\n\n",
            cfg_path, SW, SH);

    publish_clients();
    publish_ws();
    publish_title();
    publish_panel_geom();
    publish_workarea();
    tile_apply();

    run_autostart();

    /* The xrandr commands in autostart fork asynchronously; the mirror
       may not be live yet. Block briefly, then re-read the screen size
       and re-tile so the very first frame is correct. */
    if (n_auto > 0) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 700000 };
        select(0, NULL, NULL, NULL, &tv);
        read_screen_size();
        tile_apply();
        XSync(dpy, False);
    }

    XEvent ev;
    int xfd = ConnectionNumber(dpy);
    for (;;) {
        fd_set r; FD_ZERO(&r); FD_SET(xfd, &r);
        int mx = xfd;
        if (ipc_fd >= 0) { FD_SET(ipc_fd, &r); if (ipc_fd > mx) mx = ipc_fd; }
        if (select(mx + 1, &r, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ipc_fd >= 0 && FD_ISSET(ipc_fd, &r)) ipc_handle();

        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case MapRequest: {
                Window w = ev.xmaprequest.window;
                int rws = 0, rflt = -1;
                int rskip = -1;
                apply_rules(w, &rws, &rflt, &rskip);
                int target = rws ? rws : cur_ws;
                int flt = is_float(w);
                if (rflt >= 0) flt = rflt;
                if (!tiled[target]) flt = 1;   /* tiling off == everything floats */

                /* desktop/dock windows and rule-skipped windows are
                   mapped but never managed: no border, no tile, no focus,
                   no client list entry, and kept at the very bottom. */
                if (is_desktop_window(w) || rskip == 1) {
                    XSetWindowBorderWidth(dpy, w, 0);
                    XMapWindow(dpy, w);
                    XLowerWindow(dpy, w);
                    fprintf(stderr, "bawm: skipping 0x%lx (desktop/skip)\n",
                            (unsigned long)w);
                    break;
                }

                XSetWindowBorderWidth(dpy, w, bw);
                XSetWindowBorder(dpy, w, col_unfocus);
                XSelectInput(dpy, w, PropertyChangeMask |
                                    StructureNotifyMask |
                                    EnterWindowMask |
                                    FocusChangeMask);
                set_card(w, A_WMSTATE, NormalState);
                XMapWindow(dpy, w);
                XSync(dpy, False);
                /* give picom a moment to run its open animation before
                   we move the window into its tile slot. Without this,
                   the resize arrives while picom is still planning the
                   animation and picom bails. */
                usleep(120000);   /* 120 ms */
                set_ws(w, target);

                if (target != cur_ws) {
                    set_card(w, A_WMSTATE, IconicState);
                    XUnmapWindow(dpy, w);
                } else if (!in_switch) {
                    if (tiled[target] && !flt) {
                        /* if a slot was reserved for this exact window,
                           put it back there instead of inserting anew */
                        if (!revive_ghost(&tree[target], w))
                            tinsert(target, focused, w);
                    }
                    if (flt) {
                        int ww = SW * 2 / 3;
                        int wh = (SH - tile_apply_ph()) * 2 / 3;
                        XMoveWindow(dpy, w, (SW - ww)/2,
                                    tile_apply_ph() + (SH - tile_apply_ph() - wh)/2);
                        XRaiseWindow(dpy, w);
                        XFlush(dpy);
                    }
                    focus(w, 0);
                    tile_apply();
                }
                publish_clients();
                break;
            }
            case ConfigureRequest: {
                XConfigureRequestEvent *cr = &ev.xconfigurerequest;
                int is_t = (cur_ws >= 1 && cur_ws <= WS_MAX && tiled[cur_ws] &&
                            tfind(tree[cur_ws], cr->window) != NULL &&
                            !is_float(cr->window));
                int mask = cr->value_mask;
                if (is_t) mask &= ~(CWX|CWY|CWWidth|CWHeight);
                if (mask) {
                    XWindowChanges wc = {
                        .x = cr->x, .y = cr->y,
                        .width = cr->width, .height = cr->height,
                        .border_width = cr->border_width,
                        .sibling = cr->above, .stack_mode = cr->detail,
                    };
                    XConfigureWindow(dpy, cr->window, mask, &wc);
                }
                if (is_t) tile_apply();
                break;
            }
            case KeyPress:
                last_time = ev.xkey.time;
                handle_key(&ev.xkey);
                break;
            case ButtonPress: {
                last_time = ev.xbutton.time;
                unsigned int st = ev.xbutton.state & ~(LockMask|Mod2Mask);
                Window w = ev.xbutton.subwindow;
                if (!w) {
                    Window rr2, cr2; int rx, ry, wx, wy; unsigned int m;
                    XQueryPointer(dpy, root, &rr2, &cr2, &rx, &ry, &wx, &wy, &m);
                    w = cr2;
                }
                if (!w) break;
                XWindowAttributes wa;
                if (!XGetWindowAttributes(dpy, w, &wa)) break;
                if (wa.override_redirect) break;
                w = top_level(w);
                if (!w || w == root) break;
                if (st == mv_mod && ev.xbutton.button == mv_btn) {
                    tile_drag(w);
                } else if (st == rz_mod && ev.xbutton.button == rz_btn) {
                    drag_window(w, 1);
                } else if (st == fc_mod && ev.xbutton.button == fc_btn) {
                    focus(w, ev.xbutton.time);
                    XAllowEvents(dpy, ReplayPointer, ev.xbutton.time);
                }
                break;
            }
            case DestroyNotify: {
                XWindowAttributes dn_wa;
                if (XGetWindowAttributes(dpy, ev.xdestroywindow.window,
                                         &dn_wa) && dn_wa.override_redirect) {
                    /* panel, popups, preview: not ours, ignore */
                    break;
                }
                for (int ws = 1; ws <= WS_MAX; ws++)
                    if (tree[ws]) tremove(&tree[ws], ev.xdestroywindow.window);
                if (ev.xdestroywindow.window == focused) {
                    focused = None;
                    Window rr2, pr2, *kids; unsigned int nn;
                    if (XQueryTree(dpy, root, &rr2, &pr2, &kids, &nn)) {
                        for (unsigned int i = 0; i < nn; i++) {
                            XWindowAttributes wa;
                            if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
                            if (wa.override_redirect) continue;
                            if (wa.map_state != IsViewable) continue;
                            if (is_desktop_window(kids[i])) continue;
                            if (!has_prop(kids[i], A_WMDESK)) continue;
                            if (get_ws(kids[i], cur_ws) != cur_ws) continue;
                            focus(kids[i], 0); break;
                        }
                        if (kids) XFree(kids);
                    }
                }
                publish_clients();
                tile_apply();
                break;
            }
            case UnmapNotify:
                if (ev.xunmap.window == focused && !in_switch) focused = None;
                publish_clients();
                break;
            case ClientMessage: {
                XClientMessageEvent *cm = &ev.xclient;
                if (cm->message_type == A_ACTIVE) {
                    if (cm->data.l[1]) last_time = cm->data.l[1];
                    if (cm->window != None) {
                        focus(cm->window, cm->data.l[1] ? cm->data.l[1] : 0);
                        XRaiseWindow(dpy, cm->window);
                    }
                } else if (cm->message_type == A_CLOSEW) {
                    close_win(cm->window);
                } else if (cm->message_type == A_DESKTOP) {
                    do_switch_ws((int)cm->data.l[0] + 1);
                } else if (cm->message_type == A_STATE) {
                    Atom a1 = cm->data.l[1], a2 = cm->data.l[2];
                    if (a1 == A_FS || a2 == A_FS) {
                        int on = has_prop(cm->window, A_FSGEOM);
                        int want = (cm->data.l[0] == 1) ? 1 :
                                   (cm->data.l[0] == 0) ? 0 : !on;
                        do_fullscreen(cm->window, want);
                    }
                }
                break;
            }
            case PropertyNotify:
                if (ev.xproperty.window == root && ev.xproperty.atom == A_PANEL_H) {
                    publish_workarea();
                    tile_apply();
                } else if (ev.xproperty.atom == A_WMNAME &&
                           ev.xproperty.window == focused) {
                    publish_title();
                }
                break;
            case MotionNotify:
                if (focus_follows && !in_switch) {
                    Window rr, cr; int rx, ry, wx, wy; unsigned int m;
                    XQueryPointer(dpy, root, &rr, &cr, &rx, &ry, &wx, &wy, &m);
                    if (cr != None && cr != root && cr != focused) {
                        XWindowAttributes wa;
                        if (XGetWindowAttributes(dpy, cr, &wa) &&
                            !wa.override_redirect && wa.map_state == IsViewable) {
                            Window t = top_level(cr);
                            if (t && t != root && get_ws(t, cur_ws) == cur_ws)
                                focus(t, ev.xmotion.time);
                        }
                    }
                }
                break;
            case EnterNotify:
                if (focus_follows && !in_switch &&
                    ev.xcrossing.mode == NotifyNormal &&
                    ev.xcrossing.detail != NotifyInferior) {
                    Window w = ev.xcrossing.window;
                    XWindowAttributes wa;
                    if (XGetWindowAttributes(dpy, w, &wa) &&
                        !wa.override_redirect &&
                        wa.map_state == IsViewable &&
                        !is_desktop_window(w) &&
                        get_ws(w, cur_ws) == cur_ws) {
                        /* every focus change re-raises floats via
                           focus() → raise_floats(). */
                        if (w != focused) focus(w, ev.xcrossing.time);
                    }
                }
                break;
            case RRScreenChangeNotify: {
                XRRUpdateConfiguration(&ev);
                read_screen_size();
                tile_apply();
                int nw = DisplayWidth(dpy, scr);
                int nh = DisplayHeight(dpy, scr);
                if (nw != SW || nh != SH) {
                    SW = nw; SH = nh;
                    fprintf(stderr, "bawm: screen resized to %dx%d\n", SW, SH);
                    publish_panel_geom();
                    publish_workarea();
                    tile_apply();
                }
                break;
            }
            case MappingNotify:
                XRefreshKeyboardMapping(&ev.xmapping);
                if (ev.xmapping.request == MappingKeyboard) {
                    XUngrabKey(dpy, AnyKey, AnyModifier, root);
                    grab_all();
                }
                break;
            }
        }
    }
    XCloseDisplay(dpy);
    return 0;
}
