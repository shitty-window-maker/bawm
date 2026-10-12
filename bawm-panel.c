/* tinypanel.c — minimal panel for tinywm.
   build: cc -O2 tinypanel.c -o tinypanel -lX11
   click a workspace number to switch. status comes from _BAWM_STATUS
   on the root window; set it with xprop from a shell script. */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_MAX 9

static Display *dpy;
static int scr, sw, ph = 20;
static Window root, win;
static GC gc;
static XFontStruct *font;
static Atom A_WS, A_TITLE, A_PH, A_CUR, A_STATUS, A_CLIENTS, A_DESKTOP;

static int ws_x[WS_MAX + 1];
static int ws_w[WS_MAX + 1];

static unsigned long bg = 0x1a1a1a, fg = 0xd8d8d8,
                     accent = 0xc97064, dim = 0x555555;

static char *dup_n(const char *s, size_t n) {
    char *r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n); r[n] = 0; return r;
}
static char *get_str(Window w, Atom a) {
    Atom at; int af; unsigned long ni, ba; unsigned char *d = NULL;
    char *r = NULL;
    if (XGetWindowProperty(dpy, w, a, 0, 4096, False, AnyPropertyType,
                           &at, &af, &ni, &ba, &d) == Success) {
        if (d && ni > 0) r = dup_n((char*)d, ni);
        if (d) XFree(d);
    }
    return r;
}
static unsigned int ws_occupied_mask(void) {
    Atom at; int af; unsigned long ni, ba; unsigned char *d = NULL;
    unsigned int mask = 0;
    if (XGetWindowProperty(dpy, root, A_CLIENTS, 0, 256, False, XA_WINDOW,
                           &at, &af, &ni, &ba, &d) == Success && d) {
        Window *wins = (Window*)d;
        for (unsigned long i = 0; i < ni; i++) {
            Window w = wins[i];
            Atom t2; int af2; unsigned long n2, b2; unsigned char *dd = NULL;
            if (XGetWindowProperty(dpy, w, A_DESKTOP, 0, 1, False,
                                   XA_CARDINAL, &t2, &af2, &n2, &b2, &dd)
                == Success) {
                if (dd && n2 >= 1) {
                    long ws = ((long*)dd)[0] + 1;   /* 1-based */
                    if (ws >= 1 && ws <= WS_MAX) mask |= (1u << (ws - 1));
                }
                if (dd) XFree(dd);
            }
        }
        XFree(d);
    }
    return mask;
}

static long get_card(Window w, Atom a, long def) {
    Atom at; int af; unsigned long ni, ba; unsigned char *d = NULL;
    long r = def;
    if (XGetWindowProperty(dpy, w, a, 0, 1, False, AnyPropertyType,
                           &at, &af, &ni, &ba, &d) == Success) {
        if (d && ni >= 1) r = ((long*)d)[0];
        if (d) XFree(d);
    }
    return r;
}
static void draw(void) {
    int cur = (int)get_card(root, A_WS, 1);
    unsigned int occ = ws_occupied_mask();
    char *title  = get_str(root, A_TITLE);
    char *status = get_str(root, A_STATUS);
    if (!title)  title  = strdup("");
    if (!status) status = strdup("");

    int y = font->ascent + (ph - font->ascent - font->descent) / 2;
    int x = 6;

    XSetForeground(dpy, gc, bg);
    XFillRectangle(dpy, win, gc, 0, 0, sw, ph);

    /* workspaces */
    for (int i = 1; i <= WS_MAX; i++) {
        char b[8]; int n = snprintf(b, sizeof b, "%d", i);
        ws_x[i] = x;
        ws_w[i] = font->max_bounds.width;
        unsigned long col;
        int has = (occ >> (i - 1)) & 1;
        if (i == cur) col = accent;
        else if (has) col = fg;
        else          col = dim;
        XSetForeground(dpy, gc, col);
        XDrawString(dpy, win, gc, x, y, b, n);
        x += font->max_bounds.width + 6;
    }
    x += 8;

    /* status, right-aligned */
    int st_w = status[0] ? (int)strlen(status) * font->max_bounds.width : 0;
    int st_x = sw - st_w - 8;
    if (st_x < x + 20) st_x = x + 20;   /* don't collide with title */

    /* title, clipped to leave room for status */
    int max_tw = (st_x - x - 12) / font->max_bounds.width;
    if (max_tw < 0) max_tw = 0;
    int tlen = (int)strlen(title);
    if (tlen > max_tw) tlen = max_tw;
    XSetForeground(dpy, gc, fg);
    if (tlen > 0) XDrawString(dpy, win, gc, x, y, title, tlen);

    /* status text */
    if (status[0]) {
        XSetForeground(dpy, gc, fg);
        XDrawString(dpy, win, gc, st_x, y, status, strlen(status));
    }

    free(title);
    free(status);
    XFlush(dpy);
}
static void switch_to(int n) {
    XEvent ev; memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = root;
    ev.xclient.message_type = A_CUR;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = n - 1;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

static int panel_xerror(Display *d, XErrorEvent *e) {
    /* ignore BadWindow/BadDrawable: happens when a client dies between
       the WM publishing _NET_CLIENT_LIST and us reading it */
    if (e->error_code == BadWindow || e->error_code == BadDrawable ||
        e->error_code == BadMatch  || e->error_code == BadValue)
        return 0;
    char buf[256];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    fprintf(stderr, "bawm-panel: X error: %s\n", buf);
    return 0;
}
static int panel_xio(Display *d) {
    (void)d;
    fprintf(stderr, "bawm-panel: X connection lost\n");
    return 0;
}

int main(void) {
    const char *fn = getenv("BAWM_PANEL_FONT");
    if (!fn) fn = "fixed";

    XSetErrorHandler(panel_xerror);
    XSetIOErrorHandler(panel_xio);
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "bawm-panel: no display\n"); return 1; }
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    sw = DisplayWidth(dpy, scr);

    A_WS     = XInternAtom(dpy, "_BAWM_WS", False);
    A_TITLE  = XInternAtom(dpy, "_BAWM_TITLE", False);
    A_PH     = XInternAtom(dpy, "_BAWM_PANEL_HEIGHT", False);
    A_CUR    = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    A_STATUS = XInternAtom(dpy, "_BAWM_STATUS", False);
    A_CLIENTS = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    A_DESKTOP = XInternAtom(dpy, "_NET_WM_DESKTOP", False);

    font = XLoadQueryFont(dpy, fn);
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (!font) { fprintf(stderr, "bawm-panel: no font\n"); return 1; }
    ph = font->ascent + font->descent + 6;

    XSetWindowAttributes a = {
        .override_redirect = True,
        .background_pixel  = bg,
        .event_mask        = ExposureMask | PropertyChangeMask |
                             ButtonPressMask,
    };
    win = XCreateWindow(dpy, root, 0, 0, sw, ph, 0, DefaultDepth(dpy, scr),
                        CopyFromParent, DefaultVisual(dpy, scr),
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetFont(dpy, gc, font->fid);
    XMapRaised(dpy, win);

    long h = ph;
    XChangeProperty(dpy, root, A_PH, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char*)&h, 1);
    XSelectInput(dpy, root, PropertyChangeMask);
    draw();

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose) {
            if (ev.xexpose.count == 0) draw();
        } else if (ev.type == PropertyNotify &&
                   (ev.xproperty.atom == A_WS ||
                    ev.xproperty.atom == A_TITLE ||
                    ev.xproperty.atom == A_STATUS ||
                    ev.xproperty.atom == A_CLIENTS)) {
            draw();
        } else if (ev.type == ButtonPress && ev.xbutton.button == Button1) {
            for (int i = 1; i <= WS_MAX; i++) {
                if (ev.xbutton.x >= ws_x[i] &&
                    ev.xbutton.x <  ws_x[i] + ws_w[i]) {
                    switch_to(i);
                    break;
                }
            }
        }
    }
}
