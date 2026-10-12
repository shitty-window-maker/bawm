# bawm

**barely a window manager.**

Small X11 tiling WM in ~2,000 lines of C. Dwindle layout, i3-style
drag with drop preview, workspaces, EWMH, IPC socket.

## Build

Deps: `libx11`, `libxext`, `libxrandr`, `libyaml`.

    # Debian / Ubuntu
    sudo apt install libx11-dev libxext-dev libxrandr-dev libyaml-dev
    # Arch
    sudo pacman -S libx11 libxext libxrandr libyaml
    # Fedora
    sudo dnf install libX11-devel libXext-devel libXrandr-devel libyaml-devel

    make
    sudo make install

## Run

Add to `~/.xinitrc`:

    bawm-panel &
    ~/.config/bawm/status.sh &
    exec bawm

Config auto-generates at `~/.config/bawm/config.yaml` on first run.
Validate with `bawm --check`. Reload with `Super+Shift+r`.

## Keys

| Binding | Action |
|---|---|
| `Super+Return` | spawn kitty |
| `Super+d` | spawn rofi |
| `Super+Shift+c` | close focused |
| `Super+Shift+q` | quit |
| `Super+Shift+r` | reload config |
| `Super+Tab` / `Super+Shift+Tab` | cycle focus |
| `Super+t` | toggle tiling on this workspace |
| `Super+h/j/k/l` | focus tile in direction |
| `Super+Shift+h/j/k/l` | swap focused tile with neighbor |
| `Super+[` / `Super+]` | shrink / grow split |
| `Super+s` | flip parent split |
| `Super+f` | float toggle |
| `Super+Shift+f` | windowed |
| `Super+F11` | fullscreen |
| `Super+g` | toggle gaps |
| `Super+1..9` | switch workspace |
| `Super+Shift+1..9` | send focused to workspace |
| `Super+LMB` | drag tile (swap / split on drop) |
| `Super+RMB` | resize |

## Status script

The panel's right side reads `_BAWM_STATUS` on the root window:

    xprop -root -f _BAWM_STATUS 8s -set _BAWM_STATUS "$(date '+%H:%M')"

## License

MIT.
