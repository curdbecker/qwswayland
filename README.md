# QWSWayland - QWS to Wayland Protocol Proxy

A bridge that allows legacy Qt 4.8.7/QWS (Qt Window System) applications to
run unmodified under a Wayland compositor - analogous to how XWayland enables
legacy X11 applications on Wayland, but so far without a direct integration into a compositor itself.

## Architecture

![Architecture diagram](docs/architecture.svg)

Each Qt 4.8.7 application connects to the proxy over a Unix socket using the
QWS wire protocol, exchanging commands and events backed by POSIX or SysV
shared memory. The proxy translates those into Wayland operations and routes
compositor input events back to the appropriate client.

## Components

### `libqwsproto/`
Re-implementation of the QWS wire protocol without Qt dependency.

Provides parsing and serialisation of QWS commands (client→server) and
events (server→client), plus the Unix socket and shared-memory transport
layer (both SysV and POSIX IPC backends).


>The QWS wire protocol has some rough edges regarding its serialization depending on the compiler and/or the architecture - e.g. C bit fields have been used and there is no specific consideration for word-alignment.
>
>This fact may cause issues on other architectures (tested only on ARM64 so far) and might require specific adjustment depending on the Qt build that shall be interfaced with - e.g. if client blittering has been enabled in the Qt build using a header definition, it needs to be enabled in libqwsproto as well.
>
>However, this still is worth the trouble as it removes very old Qt source code dependency which may become suddenly quite fragile in modern compilers (those who know know and I am now unfortunately one of them - let me just say: `QT_NO_FOREACH` makes a lot of sense now).
>
>Therefore, this decision may cause some additional pain at a later stage, but also saves a lot of headache during the entire development process. And in any case trying to bind against the compiled, binary versions of the Qt version and static-linked applications would be likely no joke either. 
>
>I hope that maximal extend of modification to an executable at some point might be using something like `LD_PRELOAD` to affect some higher-level changes.

### `proxy/`
The QWSWayland proxy daemon. Listens for QWS client connections and:

- Maps each QWS window to a `wl_surface` with an `xdg_toplevel` (top-level
  windows) or a `wl_subsurface` (child windows).
- Copies pixel data from the QWS client's shared-memory buffer into a
  `wl_shm`-backed `wl_buffer` and commits it to the compositor.
- Routes Wayland input events (`wl_pointer`, `wl_keyboard`) back to the
  appropriate QWS client as QWS events.
- Uses `zxdg_output_manager_v1` / `zxdg_output_v1` to track the logical
  output geometry reported by the compositor.

## Building

```bash
mkdir build && cd build
cmake ..
make
```

**Dependencies:** `libwayland-client`, `libxkbcommon`, `wayland-protocols`,
`wayland-scanner`, `libicu` (icu-uc), `pkg-config`

## Usage

```bash
# Start the proxy (creates /tmp/qtembedded-<display>/QtEmbedded-<display>)
./qwswayland
```

Qt 4.8.7 applications should then be launched with `-qws`. The default display id is `0` as before. Otherwise, the `QWS_DISPLAY` environment variable should be set to point at the same display number. 

## Status

**Early prototype** — proof-of-concept quality. Core window creation,
pixel-buffer commits, keyboard/pointer input, and child-window (subsurface)
support are functional.

## Acknowledgement / Warning

AI tooling has been used in this project - specifically:

* **Claude Chat** - to create a rough first draft of the implementation to judge how complex the protocol is in the first place and see how far it will get on its own. This unfortunately lead to some quite annoying discoveries later one when Claude hallucinated some protocol/Qt-level structures that could have been specified as it thought, but just haven't been. That caught me very much off-guard as I am not familiar with Qt development at all.
* **Claude Code** - to take over active development with assistance, but this is better for control and worse for abstraction unfortunately - although it could have been due at least partially to lack of clear guidance on my part.

In general, the goal is to review AI-generated code as good as possible while hopefully refining the "AI rules of engagagement" to prevent redundant, overly complicated or simply massively overblown AI code in general. Otherwise, this even quickly seems to get Claude overwhelmed. And this makes me personally also a bit more complicated as it 
In general, the goal is to review AI-generated code as good as possible while hopefully refining the "AI rules of engagagement" to prevent redundant, overly complicated or simply massively overblown AI code in general. Otherwise, this even quickly seems to get Claude overwhelmed. And this makes me personally also a bit more comfortable.