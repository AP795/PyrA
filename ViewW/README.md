<!--
PyrA Project, Git ap795/PyrA
Copyright (c) 2026 Alexandre Papine
SPDX-License-Identifier: MIT
-->

# Localhost test viewer

`PyrA_Web_Viewer` is an optional Qt-free viewer. A native C++ process opens
images through the public PyrA C API and serves embedded HTML/CSS/JavaScript
over HTTP on `127.0.0.1`. The browser performs only interaction and display;
the native library performs image reading and region rendering.

The UI provides image selection beneath a configured directory, metadata,
channel/Z selection, pan, wheel zoom, Fit, physical-pixel 1:1, and an interactive
minimap. A vertical slider to the minimap's right selects the current channel's
available Z positions, displays Z in micrometres, and preserves pan and zoom.
It stays synchronized with the Channel/Z selector and hides for single-Z
channels. It keeps the current frame visible while the next one is rendered.
ZYP stacks initially select their central plane. Their focal positions are
shown as plane numbers because the physical Z spacing is unverified.
**Generate…** writes a synthetic bright-field z-stack
into the folder last shown by **Open image** and then opens it. Choose **PTF**
or, in a build with DCMTK, **DICOM (.dcm)**. DICOM writes one instance per
pyramid resolution with all focal planes. Open the named `.dcm` file and
keep the sibling `_<name>_/` directory with it for lower resolutions.
DICOM offers baseline JPEG, JPEG 2000 and HTJ2K when built. JPEG XL and
JPEG XR are available for PTF. Tile 0 writes a plain untiled TIFF instead
(`.tif`, one plane, LZW or JPEG, at most 16384 pixels per edge); it is not
opened, and DICOM refuses it. Its dialog
has the Qt viewer's slide, focus and tissue settings; the request sends
fractions as 0..1 (`z_step`, `focus`, `nucleus`, `spacing`, `positive`,
`tumour`, `lymphocytes`, each optional with the library default). Generation
runs in the background. `POST /api/generate` starts it with `format=ptf`
(the default) or `format=dicom`; `/api/state` lists each codec with
`tiled`/`untiled` flags. `GET /api/generate`
reports tile (or strip) progress, and `POST /api/generate-cancel` stops it. One
generation runs at a time, and an existing file name is refused.
The existing Qt evaluator remains available for recording/replay.

**Analyse…** runs the Python processing bridge
([docs/python-bridge.md](../docs/python-bridge.md)) on the open DICOM
slide and adds its result as a new channel. The dialog asks for:

- the prepare script (optional) and the process script, both chosen from the
  scripts folder;
- the new channel's name and output type, the halo, and the number of Python
  workers;
- `key=value` parameters, one per line;
- **apply by CLI** (the server starts `pyra_process`) or **apply by server**
  (the bridge runs in this process).

When the run succeeds, the slide reloads in place, keeping the view and the
selected plane, and the dialog and status bar give the timing. On failure the
dialog shows the error, with any Python traceback, and nothing is reloaded.

Server side:

- **Endpoints.** `POST /api/analyse` starts a run, `GET /api/analyse` reports
  its phase and progress and then the report, and `GET /api/scripts` lists the
  scripts. One run goes at a time; it cannot be cancelled yet.
- **Scripts folder.** Scripts are taken only from `--scripts DIR`, by plain
  file name. The default is the bridge's `python/examples`.
- **Bridge paths.** `--bridge-cli PATH` and `--bridge-worker PATH` locate
  `pyra_process` and `pyra_worker.py`, and `--python PATH` the interpreter. By
  default they are found beside the server (an install) or in the build tree.
- **Separate reader copy.** The bridge is C++ on the reader's internals, so a
  viewer built with it (`PYRA_BUILD_BRIDGE`, the default) holds its own static
  copy of the reader beside `pyra.dll`. Without the bridge, or without DICOM
  support, the button stays disabled and its tooltip says why.

The viewer uses the bundled `Deps/httplib/`. To use a different version,
set `PYRA_HTTPLIB_DIR` to a directory containing both `httplib.h` and `LICENSE`.

## Build and run on Linux

With existing codec SDKs available:

```sh
cmake -S . -B out/build/linux-web-system -DCMAKE_BUILD_TYPE=Release \
  -DPYRA_BUILD_GUI_TEST=OFF -DPYRA_BUILD_WEB_TEST=ON
cmake --build out/build/linux-web-system --parallel 2
ctest --test-dir out/build/linux-web-system --output-on-failure
out/build/linux-web-system/ViewW/PyrA_Web_Viewer --root /data/images
```

If the SDKs are not discoverable, provide the TIFF, JPEG and OpenJPEG paths
documented in the root README. `PYRA_TIFF_EXTRA_LIBRARIES` supplies additional
link libraries for custom static TIFF builds, such as a static zlib archive.

To build using the dependency sources in the checkout, run from the PyrA root:

```sh
bash ViewW/build_linux.sh
out/build/linux-web/install/bin/PyrA_Web_Viewer --root /data/images
```

The helper builds the codecs and a static DCMTK 3.7.0 subset, then enables
HTJ2K and DICOM, runs CTest, and installs the viewer. It works offline with
GCC 13 or a newer C++20 toolchain, CMake 3.20 or newer, Make and Bash.
Node enables the JavaScript tests. Python 3 and numpy enable the bridge
tests. Pass a build directory as the script's only argument to use a
different output location. See [codec coverage](../Deps/README.md).
To open a DICOM slide, select one of its `.dcm` files.

To embed the [browser client](#browser-rendering), set `PYRA_WEB_CLIENT_DIR` to
a folder holding `pvue_client.js` and `pvue_client.wasm` (an Emscripten build of `Pvue/wasm`):

```sh
PYRA_WEB_CLIENT_DIR=/path/to/client bash ViewW/build_linux.sh
```

The two files need no Emscripten on this machine: they can be copied from any
build of the client (see [Building the client](#building-the-client)).

The default image directory is `tst_in/` in the checkout used to build the
viewer, independent of its working directory. Set `PYRA_VIEWER_IMAGE_DIRECTORY`
in CMake to change this default or use `--root IMAGE_DIRECTORY` at launch; a
directory chosen in the viewer (Change, below) replaces the default until
another is chosen.

Open the printed URL (normally `http://127.0.0.1:8765/`) in a browser on the
same machine. `--port 0` selects a free port; `--image relative/path.svs` opens
an initial image. Without `--image`, each browser reopens the last image it
opened, with the same reader and cache options (kept in its `localStorage` on
every successful open; skipped when the root has changed since, forgotten when
it no longer opens). All assets are embedded, so the executable can start from
any working directory. Linux installations use an executable-relative RPATH
for `libpyra.so`.

For WSL, use distro `Client`, Linux image paths, and the Windows host browser
through WSL's localhost forwarding. For a remote VM, keep the server bound to
loopback and forward its port, for example:

```sh
ssh -L 8765:127.0.0.1:8765 user@vm
```

The standard browser file picker cannot give the server an absolute file
path. The Open image dialog therefore lists the server's configured image
directory and accepts relative paths, without uploading or copying slides.
Symlinks and parent paths resolving outside that root are rejected.

**Change**, beside the directory in the Open image dialog, moves the root while
the server runs: enter an absolute directory and Set (or Enter). Every session
then browses the new root. The server keeps it for its next start in
`web-root.txt` in its per-user state folder (`%LOCALAPPDATA%\Pyra` on Windows,
`$XDG_STATE_HOME/pyra` or `~/.local/state/pyra` on Linux, beside `web-logs`): a
start without `--root` uses it while it is still a directory, else the default
above; `--root` always wins, and the startup line says which was used. Only a
browser on the server's machine sees Change: the
root is what confines network clients, so `--allow-remote-write` does not let
them move it, and a request through a proxy is refused. The change waits for a
slide being generated or analysed. An open slide beneath the new root keeps its
path; one outside stays shown, but Analyse and reloading need it reopened.

Each
browser has its own session (a `pyra_sid` cookie) with its own open image, so two
browsers view different slides; the tabs of one browser share its session. See
[On a network](#on-a-network) for the session limits.

## Browser rendering

When the viewer is built with the browser client (below), views are rendered in
the browser. The server sends each tile as it is stored in the slide, unchanged;
the browser decodes and renders it with PyrA's own reader and Pvue's display rule
([Pvue/README.md](../Pvue/README.md)), compiled to WebAssembly, so the pixels are
the server's. The server never decodes for views, and a revisited area costs no
transfer.

- `GET /api/geometry?generation=N` returns the slide's geometry (levels and tile
  rectangles), from which the browser builds its copy of the slide
  (`pyra_open_remote`).
- `GET /api/raw-tile?generation=N&level=L&tile=T` returns one stored tile
  (`pyra_read_encoded_tile`). JPEG tables, NDPI headers and the CZI sub-block
  descriptor are included, so each tile decodes on its own. Synthetic coarse
  levels, which exist only in the server's memory, are sent as losslessly
  encoded 512-pixel tiles. Both URLs name the slide generation and are cached as
  immutable; a stale generation returns 410.
- A Web Worker (Pvue's `pvue_worker.js`) runs the client. For each view it renders
  what it holds and shows it, fetches the missing tiles (six at a time, a
  browser's connections per host), and renders again until the view is complete.
- The client keeps 256 MiB in each of its compressed and decoded caches
  (`kClientCacheMiB`). A view whose tiles exceed this cannot complete, so the
  cache must hold one view; uncompressed CZI sub-blocks are the large case.
- The client decodes on up to 8 threads (`PYRA_CLIENT_MAX_THREADS` in
  `Pvue/wasm/CMakeLists.txt`, started with the module), at most the browser's
  `navigator.hardwareConcurrency`. Threads share WebAssembly memory, which
  browsers allow only on a cross-origin isolated page: the server sends
  `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp` on every response, and all
  resources are same-origin. A page that is not isolated (a proxy dropping the
  headers) or not a secure context (plain http to a remote host) gets no client
  and is rendered by the server.
- For a plane whose codec the client lacks (JPEG XL in a client built without
  it), the server renders the view and sends it as a lossless HTJ2K RGB frame (`/api/render?encoding=htj2k`,
  header `X-Pyra-Encoding`), which the client decodes. Without the client, or
  before it is ready, the view comes from `/api/render` as raw RGBA, as before.
- **Colour from channels.** A slide whose channels include R, G and B (named
  so, any case) gets a first entry in the plane list, "Colour (R, G, B)", selected
  when the slide opens; the Z slider moves it between focal positions. The
  server sends the three monochrome channels' tiles as stored, and the browser
  composes them; the server never composes, so without the client the entry is
  absent and single planes are shown. Gallery slides list single planes only.
- **Minimap.** Once the client is ready, it renders the minimap's overview as
  well, the colour view included, from the coarsest tiles; before that, or
  without the client, the server renders it.
- **Server caches.** The server sends stored tiles without decoding or
  composing, so with the native reader it keeps no decoded-tile cache: PBF is 0
  (the Settings default; a request without `pbf_mib` gets it too). The TBF
  default stays, and the tiles sent are read through it: a tile asked again is
  copied from the cache rather than read from the file. A tile that finds no
  room is still sent, read past the cache, and counted as bypassed (see the
  Caches panel). Missing coarse levels are
  built through it when a slide opens, and a gallery member must fit it. OpenSlide has no stored
  tiles to send, so the server renders and PBF keeps its default. Where the
  browser client does not run (plain http on a LAN, the Claude app's browser
  pane), the server's fallback renders decode afresh for every view: measured on
  40 pans, 7.5x slower than with a 32 MiB PBF on a JPEG 2000 SVS and 8x on a
  JPEG one; a larger server PBF can be set in Settings there.
- **Caches panel.** Above Metadata, the server's and the browser's tile caches
  (TBF compressed, PBF decoded) show the bytes each has in use of its budget
  (`Pvue/pvue_cache.h`: the bytes the cache holds for its tiles - a TBF tile
  at its own encoded size, a PBF tile in a slot of its level's tile size; tiles
  of levels the server synthesized
  are held outside the caches and not counted), refreshed
  every second while a slide is open and the page is visible (`GET /api/cache`,
  which never waits for a render; the client through `PvueClient.cacheUsage`).
  After the bytes come the tiles it holds ("448 tiles"), how full it is ("25%
  full") and its hit ratio
  over the tile lookups since the image was opened ("99.7% hits", rounded down
  so a cache that missed never reads 100%; the tooltip gives the counts; absent
  before the first lookup and for OpenSlide). A TBF is looked up only when the
  PBF lacks the decoded tile, so a browser TBF with a large PBF shows few
  lookups. A TBF stores each tile at its own size, so its bytes in use are the
  tiles' own and the tooltip gives the average tile (on CMU-1 a TBF holds about
  13 times as many tiles per MiB as when every tile took a slot sized to the
  largest).
  A server TBF scores hits only when a tile is asked again, which a running
  browser client rarely does: its own caches and the HTTP cache serve repeats.
  "Tiles read from file" is the server's, for the open image; Render
  timing's "Total sent" counts the stored tiles the server sent for it and their
  MiB. A tile sent again
  from the TBF raises the tiles sent and not the tiles read.
  "off" is a zero budget. A server TBF that had to send tiles past itself adds
  "· N bypassed", marked: its budget is too small for the workflow, so give the
  server a larger TBF in Settings. The browser rows say "starting…" until the client
  runs, "not running" if it failed to start (the status bar gives the reason),
  and "no browser client" when the build has none.
  The browser's HTTP cache is not counted.
- **Settings.** The gear button on the toolbar opens the cache settings: for
  the server and for the browser client, the TBF and PBF sizes in MiB, the cache
  type (Arena: one pool per cache for the whole slide; Per level: one pool per
  level, sized to its tiles) and a reset checkbox per cache. They replace the
  Open form's former TBF and PBF fields, and this browser remembers them
  (`pyra.cacheSettings`; a value left at its default stays the default, so the
  server PBF keeps following the reader: 0 for the native reader, the reader
  default for OpenSlide). A new size or type reopens the open image (`/api/open`
  `tbf_mib`, `pbf_mib`, `topology`), keeping the view, with every cache empty.
  Otherwise the checked caches are reset in place (`POST /api/cache/clear` with
  `compressed=1` / `decoded=1`, and the client's `clear`; `pyra_clear_cache`):
  emptied, with their hit counts restarted. A server reset leaves the browser's
  HTTP cache as it is, so tiles the browser holds are not asked for again.
  OpenSlide keeps its own cache: only its PBF size applies. The browser column
  needs a running client; each browser cache is at most `kClientCacheMaxMiB`
  (768 MiB, so both fit the client's 2 GB memory).
- **Side panel.** Drag the bar between the image and the panel (or focus it and
  use the arrow keys) to resize the panel; double-click it for the default
  360 px. The minimap grows with it. This browser remembers the width
  (`pyra.sideWidth`); a drag stops at 240 px and at 60% of the window, and a
  narrower window applies the 60% cap without changing the width remembered.

Measured on this sequence (9 views of 1920x1080; CMU-1 JPEG and JPEG 2000,
3DHISTECH DICOM, NDPI, two PTF): the client's first pass transfers 1.3-9.4 MiB
where server rendering sent 71.2 MiB, and a repeated pass transfers nothing.
Eight decode threads cut client render time by 1.7-3.5x against one (JPEG 2000
4.6 s to 1.3 s). OpenJPEG runs on the calling thread in WebAssembly
(`kOpjCodecThreads`): its per-tile thread pool needed a Web Worker started by the
main thread for every tile and had held the speedup to 1.4x. HTJ2K frames are 3.7-21x smaller than raw RGBA and cost the
server 41-69 ms of encoding each, so on loopback raw frames arrive sooner; the
compressed frame pays off once the link is slower than about 100 MB/s.

The client decodes JPEG, JPEG 2000, HTJ2K, JPEG XL, JPEG XR, TIFF's internal
codecs and CZI sub-blocks (JPEG XR, zstd), i.e. everything the server decodes
except DICOM-specific codecs it does not bundle. Checked against the server's
own `/api/render`: identical for SVS JPEG and JPEG 2000, DICOM, NDPI and PTF
(lossless JPEG XL, JPEG XR, HTJ2K). Lossy HTJ2K differs by one code value in a
few samples per million and lossy JPEG XL by one code value in about 1% of
samples: the rounding of float decoders in WebAssembly versus x64 SIMD. On a
synthetic PTF, the four-view pass that the server fallback answered with
2.5-2.8 MiB of HTJ2K frames, every time, costs the JPEG XL client 1.7-4.3 MiB of
stored tiles once and nothing on a repeat; adding JPEG XL grew
the client from 2.30 to 3.06 MB.

### Building the client

The client is Pvue's (`Pvue/wasm`), a separate Emscripten build of this tree;
[Pvue/README.md](../Pvue/README.md#building-the-client) gives the steps. Configure
the native viewer with `-DPYRA_WEB_CLIENT_DIR=<wasm build>/Pvue/wasm`: the server
embeds `pvue_client.js` and `pvue_client.wasm` and stays one self-contained
executable. The pair needs no Emscripten on the viewer's machine: copy both files
together, with the `third-party/` directory beside them, which holds the notices of
libjxl and the libraries it bundles; the viewer installs them under
`share/Pyra/third-party/`. The native build embeds the client but never
rebuilds it; it warns when an embedded client built from this tree is older than
its sources ([Pvue/README.md](../Pvue/README.md#building-the-client)).

## On a network

The server listens on `127.0.0.1` only. To reach it from other machines, put a
TLS reverse proxy such as Caddy in front of it on the same host, and name each
address the proxy serves with `--public-origin`, once per address, for example
the host name and the IP address:

```sh
PyrA_Web_Viewer --root /data/images --port 8765 \
  --public-origin https://viewer.lan --public-origin https://192.168.1.20
```

```
viewer.lan, 192.168.1.20 {
	tls internal
	reverse_proxy 127.0.0.1:8765
}
```

HTTPS is required, not only advisable: browsers give the decode threads'
shared memory only to a cross-origin isolated page in a secure context, and a
plain-http address other than localhost is not one. Such a page still works,
with every view rendered by the server. The server's COOP/COEP headers pass
through the proxy unchanged. `tls internal` makes Caddy issue the certificate
from its own authority, whose root each client must trust (`caddy trust` on the
proxy host; import the root certificate on other machines and phones). A
certificate from a public authority needs no client step.

- **Access key.** With `--public-origin`, the server prints one access URL per
  origin, `https://viewer.lan/?key=...`, random for each run. A browser opens it
  once: the key is exchanged for an `pyra_access` cookie (another random value,
  not the key) and the page loads. A network request without that cookie is
  refused, page and assets included. Restarting the server invalidates every
  access cookie.
- **What counts as local.** A request is local when it names the loopback Host
  and Origin and carries no `X-Forwarded-For`, `X-Forwarded-Host` or `Forwarded`
  header. The proxy must add one of them, as Caddy, nginx and IIS ARR do by
  default, so a proxied request that names the loopback host is refused instead
  of being taken as local.
- **Writing.** Network clients may browse, open and view, but not generate,
  make a gallery or analyse: those requests are refused, and the buttons are
  disabled with the reason. `--allow-remote-write` lets them. One generation and
  one analysis run at a time per server, whichever session started it.
- **Sessions.** Each browser session holds its own reader and caches, sized by
  its Open dialog, so memory grows with the number of sessions. At most 8 are
  open (`kMaxSessions` in `web_server.h`). When a new browser arrives at the
  limit, sessions unused for 30 minutes (`kSessionIdleMinutes`) are closed;
  if none is, the page answers 503. The session cookie and every API request's
  page token (`X-Pyra-Session`) must match, and cookies are `HttpOnly`,
  `SameSite=Strict`, and `Secure` behind an https origin.
- **Tile URLs** name a generation that is unique across sessions, so a
  browser's HTTP cache never serves one slide's tiles for another.

## Rendering and limits

Rendering runs on HTTP worker threads, separate from browser interaction.
Each rendered frame is the view plus a halo band on every side, capped per axis
at the 4096-pixel limit. The toolbar's **Halo** field sets the band as a
percentage of the view's raster width, 0-50% (0 renders only the visible view);
a change re-renders the view at once, and each browser remembers its own value.
The server's `--view-halo PERCENT` sets the default (20%, `kViewHaloDefaultPercent`
in `web_server.h`), sent to the page with `/api/state`. Panning reuses the frame shown, or the one in flight, until the
view has used up half of its actual halo on any side; a frame centred on the view
is then requested, so it normally arrives before the halo's edge comes into
view. Navigation schedules at most one render per 35 ms (`kRenderDelayMs`) and
does not wait for the motion to stop. At 20% a 16:9 view renders about 2.4 times
its own pixels, at 10% about 1.6 times.
The UI retains only the latest pending view and allows one native request at
a time. The server serializes open/render access per session, bounds its HTTP worker
queue, and checks an image generation on each render. The browser discards
results after plane/image changes; a frame of an earlier view of the same plane
is still shown, placed by its own rectangle. An interrupted HTTP request cannot
cancel an already-running native render. Tile and geometry requests wait for the
slide instead of being refused, and the server runs 32 HTTP workers with a 5 s
keep-alive (`kHttpWorkerThreads`, `kKeepAliveSeconds`): cpp-httplib holds a
worker per open connection, so a request queued behind idle connections waits
for the keep-alive timeout. A browser keeps six connections per host, but other
clients open more while idle ones linger (Node's fetch: 55 s for a pass that
takes 1.4 s with 32 workers).

Each response contains top-down RGBA8 display pixels, without image
compression. Native RGB8/Gray8 samples keep their values; wider samples are
scaled using the plane's advertised maximum sample value, as in the Qt
viewer. Raw scientific pixels remain available through the native C API.
No additional image encoder or JSON library is needed.

Both raster axes are limited to 4096 pixels; the minimap's maximum edge is
256. Ordinary viewport requests match physical display pixels (plus the halo); exceptionally
large windows use a coarser bounded raster. A 4096-square display frame is
64 MiB, plus the native sample buffer, display conversion, HTTP and browser
copies. These limits bound caller-owned rasters, not overall process memory.
The regular TIFF/IPX loader can still decode its whole-image tile, as before.

Native render timing measures only `pyra_get_image`, excluding buffer
allocation, display conversion and transport. Request + display timing adds
HTTP, browser conversion and canvas drawing; it does not measure compositor
presentation latency. Minimap renders can warm the shared native tile cache.
These values are not a measured speed comparison with the Qt viewer.

1:1 maps one native source pixel to one physical canvas pixel, accounting for
browser device-pixel ratio. Keyboard: Ctrl+O opens, F fits, 1 selects native
resolution, +/- zoom, and arrow keys pan.

## Logs and checks

Each run writes a flushed UTF-8 session log of opens, HTTP statuses and native
render timings. Linux defaults to `$XDG_STATE_HOME/pyra/web-logs` or
`~/.local/state/pyra/web-logs`; Windows uses `%LOCALAPPDATA%/Pyra/web-logs`.
Override with `--log-dir DIRECTORY`. Native library stderr remains on the
launch terminal. Ctrl+C / SIGTERM stop the HTTP service cleanly.

The server restricts Host/Origin, requires each session's cookie and page token for API requests,
and serves only its embedded assets and configured image root. TLS and
optional cpp-httplib compression SDKs are disabled. The pinned header is
cpp-httplib 0.36.0 from the uploaded archive, SHA-256
`0e3802d09922e38012dc25c9408d43473c4a938a4b55592d4c6cff21bbfff586`.

CTest `pyra_web_server` exercises real HTTP routes, RGB8 pixels, RGB16 display
range, bounded overview/render sizes, stale generations, and local access
checks using generated native fixtures. Browser interaction and real-slide
validation are separate manual checks.
