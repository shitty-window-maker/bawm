/* bawm-ctl — talk to a running bawm over its unix socket.
   usage: bawm-ctl workspace 3
          bawm-ctl focus right
          bawm-ctl spawn firefox
          bawm-ctl query title */
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static int try_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr*)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: bawm-ctl <command> [args...]\n");
        return 2;
    }
    const char *xdg  = getenv("XDG_RUNTIME_DIR");
    const char *home = getenv("HOME");
    char paths[3][256];
    int n = 0;
    if (xdg && *xdg)  snprintf(paths[n++], 256, "%s/bawm.sock", xdg);
    if (home && *home) snprintf(paths[n++], 256, "%s/.bawm.sock", home);
    snprintf(paths[n++], 256, "/tmp/bawm-%d.sock", (int)getuid());

    int fd = -1;
    for (int i = 0; i < n; i++) {
        fd = try_connect(paths[i]);
        if (fd >= 0) break;
    }
    if (fd < 0) {
        fprintf(stderr, "bawm-ctl: no bawm socket (tried:");
        for (int i = 0; i < n; i++) fprintf(stderr, " %s", paths[i]);
        fprintf(stderr, ")\n");
        return 1;
    }

    char buf[1024]; int off = 0;
    for (int i = 1; i < argc; i++) {
        int w = snprintf(buf + off, sizeof(buf) - off, "%s%s",
                         i == 1 ? "" : " ", argv[i]);
        if (w < 0 || off + w >= (int)sizeof buf) break;
        off += w;
    }
    buf[off++] = '\n';
    if (write(fd, buf, off) < 0) { perror("write"); return 1; }

    char resp[4096];
    ssize_t r;
    while ((r = read(fd, resp, sizeof(resp) - 1)) > 0) {
        resp[r] = 0;
        fputs(resp, stdout);
    }
    close(fd);
    return 0;
}
