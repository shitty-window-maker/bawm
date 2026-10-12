CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local
DEPS     = -lX11 -lXext -lXrandr -lyaml

all: bawm bawm-panel bawm-ctl

bawm: bawm.c
	$(CC) $(CFLAGS) -o $@ $< $(DEPS)

bawm-panel: bawm-panel.c
	$(CC) $(CFLAGS) -o $@ $< -lX11

bawm-ctl: bawm-ctl.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	install -Dm755 bawm       $(DESTDIR)$(PREFIX)/bin/bawm
	install -Dm755 bawm-panel $(DESTDIR)$(PREFIX)/bin/bawm-panel
	install -Dm755 bawm-ctl   $(DESTDIR)$(PREFIX)/bin/bawm-ctl
	install -Dm644 README.md  $(DESTDIR)$(PREFIX)/share/doc/bawm/README.md

clean:
	rm -f bawm bawm-panel bawm-ctl

.PHONY: all install clean
