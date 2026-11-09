#!/bin/sh
# install.sh — build, install, and wire up a LightDM session for bawm.
#
# usage:  sudo ./install.sh            # full install
#         sudo ./install.sh --uninstall
#
# Assumes you already have the deps from README.md:
#   libx11 libxext libxrandr libyaml (+ -dev on Debian/Fedora)
set -eu

PREFIX=${PREFIX:-/usr/local}
BINDIR="$PREFIX/bin"
SESSION_DIR=/usr/share/xsessions
SESSION_FILE="$SESSION_DIR/bawm.desktop"

# Who owns the per-user config? If run via sudo, target the invoking user.
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
    REAL_USER=$SUDO_USER
    REAL_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
else
    REAL_USER=${USER:-root}
    REAL_HOME=${HOME:-/root}
fi

say()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m==>\033[0m %s\n' "$*" >&2; exit 1; }

need_root() {
    [ "$(id -u)" -eq 0 ] || die "run as root (sudo ./install.sh)"
}

# ------------------------------------------------------------------ #
# uninstall
# ------------------------------------------------------------------ #
if [ "${1:-}" = "--uninstall" ]; then
    need_root
    say "removing binaries"
    rm -f "$BINDIR/bawm" "$BINDIR/bawm-panel" "$BINDIR/bawm-ctl"
    say "removing LightDM session"
    rm -f "$SESSION_FILE"
    say "done. (config left at ~/.config/bawm)"
    exit 0
fi

# ------------------------------------------------------------------ #
# deps
# ------------------------------------------------------------------ #
say "checking build dependencies"
have_pkg() { pkg-config --exists "$1" 2>/dev/null; }
missing=""
have_pkg x11        || missing="$missing libx11-dev"
have_pkg xext       || missing="$missing libxext-dev"
have_pkg xrandr     || missing="$missing libxrandr-dev"
have_pkg yaml-0.1   || missing="$missing libyaml-dev"
if [ -n "$missing" ]; then
    warn "missing pkg-config modules. on Debian/Ubuntu install:$missing"
    warn "(on Arch: libx11 libxext libxrandr libyaml)"
    die  "install deps and re-run."
fi

# ------------------------------------------------------------------ #
# build
# ------------------------------------------------------------------ #
say "building"
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -Wall -Wextra}
$CC $CFLAGS -o bawm        bawm.c        -lX11 -lXext -lXrandr -lyaml || die "bawm build failed"
$CC $CFLAGS -o bawm-panel  bawm-panel.c  -lX11                          || die "panel build failed"
$CC $CFLAGS -o bawm-ctl    bawm-ctl.c    -lX11                          || die "ctl build failed"
say "build ok"

# ------------------------------------------------------------------ #
# install
# ------------------------------------------------------------------ #
need_root
say "installing binaries to $BINDIR"
install -d "$BINDIR"
install -m 0755 bawm        "$BINDIR/bawm"
install -m 0755 bawm-panel  "$BINDIR/bawm-panel"
install -m 0755 bawm-ctl    "$BINDIR/bawm-ctl"

# ------------------------------------------------------------------ #
# LightDM session
# ------------------------------------------------------------------ #
# LightDM reads .desktop files from /usr/share/xsessions (and
# /usr/local/share/xsessions). We write one whose Exec runs the WM
# after starting the panel and any user status script.
say "writing LightDM session $SESSION_FILE"
install -d "$SESSION_DIR"
cat >"$SESSION_FILE" <<EOF
[Desktop Entry]
Name=bawm
Comment=barely a window manager
Exec=bawm-session
TryExec=bawm-session
Type=Application
DesktopNames=bawm
EOF

# The session launcher: panel + status + WM.
say "writing session launcher $BINDIR/bawm-session"
cat >"$BINDIR/bawm-session" <<'EOF'
#!/bin/sh
# bawm-session — start panel + status, then exec the WM.
# LightDM calls this via the .desktop Exec line.

# Make sure our binaries are on PATH even in a minimal session env.
PATH="/usr/local/bin:/usr/bin:/bin:$PATH"
export PATH

# Let X clients talk to the session (xhost not strictly needed for
# same-user clients, but harmless).
#xhost +SI:localuser:"$USER" >/dev/null 2>&1 || true

# Status script (sets _BAWM_STATUS on root); optional.
[ -x "$HOME/.config/bawm/status.sh" ] && \
    "$HOME/.config/bawm/status.sh" &

# Panel.
bawm-panel &
PANEL_PID=$!

# Clean up the panel if the WM exits.
trap 'kill $PANEL_PID 2>/dev/null' EXIT

# The WM (replaces this shell; trap won't run on exec, so reap panel
# explicitly by not exec'ing — instead wait on bawm and then exit).
bawm
exit 0
EOF
chmod 0755 "$BINDIR/bawm-session"

# ------------------------------------------------------------------ #
# LightDM config sanity: make sure a greeter is installed.
# ------------------------------------------------------------------ #
if ! command -v lightdm >/dev/null 2>&1; then
    warn "lightdm not found on this system."
    warn "install it (e.g. 'apt install lightdm' / 'pacman -S lightdm lightdm-gtk-greeter')"
    warn "and enable it: 'systemctl enable lightdm'."
else
    say "lightdm detected"
    if ! systemctl is-enabled lightdm >/dev/null 2>&1; then
        warn "lightdm is not enabled as display-manager."
        warn "enable it with:  sudo systemctl enable lightdm"
        warn "(if you use another DM, configure it to start X with bawm-session instead)"
    fi
fi

# ------------------------------------------------------------------ #
# per-user config + autostart (run as the real user, not root)
# ------------------------------------------------------------------ #
say "creating default config for $REAL_USER"
sudo -u "$REAL_USER" env HOME="$REAL_HOME" \
    "$BINDIR/bawm" --check >/dev/null 2>&1 || \
    warn "config auto-create failed (will be created on first run)"

# ------------------------------------------------------------------ #
# done
# ------------------------------------------------------------------ #
cat <<EOF

\033[1;32mInstall complete.\033[0m

  binaries : $BINDIR/bawm, bawm-panel, bawm-ctl, bawm-session
  session  : $SESSION_FILE
  config   : $REAL_HOME/.config/bawm/config.yaml (created on first run)

Next steps:
  1. If LightDM isn't your display manager, either:
       sudo systemctl enable lightdm
     or add this to ~/.xinitrc and start X yourself:
       exec $BINDIR/bawm-session
  2. Log out, then pick the "bawm" session at the LightDM greeter.
  3. Tweak config at ~/.config/bawm/config.yaml; reload with Super+Shift+r.

EOF
