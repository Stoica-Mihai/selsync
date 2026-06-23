# selsync

Mirror the **PRIMARY** selection (text you highlight) into the **CLIPBOARD**, so
highlighting becomes `Ctrl+V`-pastable everywhere — without holding `Ctrl+C`.

Self-contained: it speaks the Wayland data-control protocol (or X11 XFIXES)
**directly**. It does **not** spawn `wl-copy`/`wl-paste`/`xclip`/`xsel`/`clipnotify`.
The only runtime dependencies are the system Wayland/X11 client libraries that any
graphical session already has.

## Backends (chosen automatically at runtime)

| Session | Protocol | Compositors |
|---|---|---|
| Wayland | `ext-data-control-v1` (preferred) | niri, KWin, recent wlroots |
| Wayland | `wlr-data-control-unstable-v1` v2 (fallback) | Sway, Hyprland, river, Wayfire, older wlroots |
| X11 | XFIXES + ICCCM selection ownership | any Xorg WM |

Picked via `WAYLAND_DISPLAY` first, then `DISPLAY`.

## Build / install

```sh
./install.sh        # build + install binary + enable the user service
# or, build only:
./build.sh          # produces ./selsync
```

Build requirements: a C compiler, `wayland-scanner`, and `wayland-client` dev
headers. The X11 backend is compiled in automatically when `xcb` + `xcb-xfixes`
dev packages are present (otherwise it builds Wayland-only). Protocol XMLs are
bundled under `protocols/`, so no system protocol packages are needed.

Arch: `base-devel wayland wayland-protocols libxcb`
Debian/Ubuntu: `build-essential libwayland-dev libwayland-bin libxcb1-dev libxcb-xfixes0-dev`
Fedora: `gcc wayland-devel libxcb-devel`

## Notes / limitations

- **GNOME/Mutter on Wayland exposes no data-control protocol**, so no client
  (this tool, `wl-clipboard`, `cliphist`, …) can manage the clipboard there.
  Fallbacks for GNOME-on-Wayland:
  - **Log into "GNOME on Xorg"** at the display manager (gear icon on the login
    screen) — selsync's X11 backend then works unchanged. This is the only way
    to get the exact highlight→clipboard mirror under GNOME.
  - **GNOME Shell extension** — only code running inside Mutter can touch the
    clipboard on GNOME-Wayland. Clipboard *history* extensions like
    [Clipboard Indicator](https://extensions.gnome.org/extension/779/clipboard-indicator/)
    or [Pano](https://extensions.gnome.org/extension/5278/pano/) exist, but they
    track copies, they do **not** auto-mirror PRIMARY→CLIPBOARD; no well-known
    extension replicates selsync's behaviour. The restriction is in Mutter, not
    in this tool.
- Text selections only (matches the original behaviour). Empty selections and
  cleared highlights leave the clipboard untouched.
- X11 backend does not implement INCR (chunked) transfers; very large
  selections are skipped with a warning. Normal highlight sizes are unaffected.
