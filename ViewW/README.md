<!--
PyrA Project, Git ap795/PyrA
Copyright (c) 2026 Alexandre Papine
SPDX-License-Identifier: MIT
-->

# Localhost test viewer

`pyra_web_test` is an optional Qt-free viewer. A native C++ process opens
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
DICOM offers baseline JPEG, JPEG 2000 and HTJ2K when built. JPEG XL is
available for PTF. Its dialog
has the Qt viewer's slide, focus and tissue settings; the request sends
fractions as 0..1 (`z_step`, `focus`, `nucleus`, `spacing`, `positive`,
`tumour`, `lymphocytes`, each optional with the library default). Generation
runs in the background. `POST /api/generate` starts it with `format=ptf`
(the default) or `format=dicom`. `GET /api/generate`
reports tile progress, and `POST /api/generate-cancel` stops it. One
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
out/build/linux-web-system/ViewW/pyra_web_test --root /data/images
```

If the SDKs are not discoverable, provide the TIFF, JPEG and OpenJPEG paths
documented in the root README. `PYRA_TIFF_EXTRA_LIBRARIES` supplies additional
link libraries for custom static TIFF builds, such as a static zlib archive.

To build using the dependency sources in the checkout, run from the PyrA root:

```sh
bash ViewW/build_linux.sh
out/build/linux-web/install/bin/pyra_web_test --root /data/images
```

The helper builds the codecs and a static DCMTK 3.7.0 subset, then enables
HTJ2K and DICOM, runs CTest, and installs the viewer. It works offline with
GCC 13 or a newer C++20 toolchain, CMake 3.20 or newer, Make and Bash.
Node enables the JavaScript tests. Python 3 and numpy enable the bridge
tests. Pass a build directory as the script's only argument to use a
different output location. See [codec coverage](../Deps/README.md).
To open a DICOM slide, select one of its `.dcm` files.

To embed the [browser client](#browser-rendering), set `PYRA_WEB_CLIENT_DIR` to
a folder holding `pyra_client.js` and `pyra_client.wasm`:

```sh
PYRA_WEB_CLIENT_DIR=/path/to/client bash ViewW/build_linux.sh
```

The two files need no Emscripten on this machine: they can be copied from any
build of the client (see [Building the client](#building-the-client)).

The default image directory is `tst_in/` in the checkout used to build the
viewer, independent of its working directory. Set `PYRA_VIEWER_IMAGE_DIRECTORY`
in CMake to change this default or use `--root IMAGE_DIRECTORY` at launch.

Open the printed URL (normally `http://127.0.0.1:8765/`) in a browser on the
same machine. `--port 0` selects a free port; `--image relative/path.svs` opens
an initial image. All assets are embedded, so the executable can start from
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
Symlinks and parent paths resolving outside that root are rejected. This is
a single-viewer process: tabs share its one active image/context.

## Browser rendering

When the viewer is built with the browser client (below), views are rendered in
the browser. The server sends each tile as it is stored in the slide, unchanged;
the browser decodes and renders it with PyrA's own reader, compiled to
WebAssembly, so the pixels are the server's. The server no longer decodes for
views, and a revisited area costs no transfer.

- `GET /api/geometry?generation=N` returns the slide's geometry (levels and tile
  rectangles), from which the browser builds its copy of the slide
  (`pyra_open_remote`).
- `GET /api/raw-tile?generation=N&level=L&tile=T` returns one stored tile
  (`pyra_read_encoded_tile`). JPEG tables, NDPI headers and the CZI sub-block
  descriptor are included, so each tile decodes on its own. Synthetic coarse
  levels, which exist only in the server's memory, are sent as losslessly
  encoded 512-pixel tiles. Both URLs name the slide generation and are cached as
  immutable; a stale generation returns 410.
- A Web Worker (`client_worker.js`) runs the client. For each view it renders
  what it holds and shows it, fetches the missing tiles (six at a time, a
  browser's connections per host), and renders again until the view is complete.
- The client keeps 256 MiB in each of its compressed and decoded caches
  (`kClientCacheMiB`). A view whose tiles exceed this cannot complete, so the
  cache must hold one view; uncompressed CZI sub-blocks are the large case.
- The client decodes on up to 8 threads (`PYRA_CLIENT_MAX_THREADS` in
  `wasm/CMakeLists.txt`, started with the module), at most the browser's
  `navigator.hardwareConcurrency`. Threads share WebAssembly memory, which
  browsers allow only on a cross-origin isolated page: the server sends
  `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp` on every response, and all
  resources are same-origin. A page that is not isolated (a proxy dropping the
  headers) or not a secure context (plain http to a remote host) gets no client
  and is rendered by the server.
- For a plane whose codec the client lacks (JPEG XL), the server renders the
  view and sends it as a lossless HTJ2K RGB frame (`/api/render?encoding=htj2k`,
  header `X-Pyra-Encoding`), which the client decodes. Without the client, or
  before it is ready, the view comes from `/api/render` as raw RGBA, as before.
  The minimap overview always does.

Measured on this sequence (9 views of 1920x1080; CMU-1 JPEG and JPEG 2000,
3DHISTECH DICOM, NDPI, two PTF): the client's first pass transfers 1.3-9.4 MiB
where server rendering sent 71.2 MiB, and a repeated pass transfers nothing.
Eight decode threads cut client render time by 1.7-3.5x against one (JPEG 2000
4.6 s to 1.3 s). OpenJPEG runs on the calling thread in WebAssembly
(`kOpjCodecThreads`): its per-tile thread pool needed a Web Worker started by the
main thread for every tile and had held the speedup to 1.4x. HTJ2K frames are 3.7-21x smaller than raw RGBA and cost the
server 41-69 ms of encoding each, so on loopback raw frames arrive sooner; the
compressed frame pays off once the link is slower than about 100 MB/s.

The client decodes everything the server decodes except JPEG XL (not yet built
for WebAssembly) and DICOM-specific codecs it does not bundle. Checked against
the server's own `/api/render`: identical for SVS JPEG and JPEG 2000, DICOM,
NDPI and PTF; lossy HTJ2K differs by one code value in a few samples per
million, the rounding of OpenJPH's float wavelet in WebAssembly versus x64.

### Building the client

The client is a separate Emscripten build of this tree (Windows paths shown;
any host with emsdk works):

```powershell
. C:\c\emsdk\emsdk_env.ps1
emcmake cmake -G Ninja -S Deps -B ../out/Pyra/build/wasm-deps -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../out/Pyra/build/wasm-deps/install
cmake --build ../out/Pyra/build/wasm-deps
cmake --install ../out/Pyra/build/wasm-deps
emcmake cmake -G Ninja -S . -B ../out/Pyra/build/wasm -DCMAKE_BUILD_TYPE=Release -DPYRA_LINUX_SDK=../out/Pyra/build/wasm-deps/install -DPYR_WITH_OPENJPH=ON -DPYR_WITH_JXL=OFF -DPYR_WITH_DCMTK=OFF -DPYR_WITH_CZI=OFF -DPYRA_WITH_OPENSLIDE=OFF -DPYRA_BUILD_BRIDGE=OFF -DPYRA_BUILD_WEB_TEST=OFF -DPYRA_BUILD_GUI_TEST=OFF -DBUILD_TESTING=OFF -DPYRA_ENABLE_IPO=OFF -DPYRA_ENABLE_AVX2=OFF
cmake --build ../out/Pyra/build/wasm --target pyra_client
```

It was developed with emsdk 4.0.20. Then configure the native viewer with
`-DPYRA_WEB_CLIENT_DIR=<wasm build>/ViewW/wasm`: the server embeds
`pyra_client.js` and `pyra_client.wasm` and stays one self-contained executable.
The embedding needs only CMake, so the pair can be built once and copied to a
machine without Emscripten. Copy both files together, since each `.js` loads only
its own `.wasm`. Rebuild them after changing what the client contains:
- the geometry and tile formats (`Core/remote_pyramid.*`, the CZI sub-block
  descriptor, synthetic-tile encodings);
- the exports `client_worker.js` calls;
- decoding or rendering code.

A geometry version mismatch makes the client refuse the slide, and the server
renders instead.
`node ViewW/wasm/pyra_client_test.cjs <wasm build>/ViewW/wasm/pyra_client.js
http://127.0.0.1:PORT SLIDE...` compares the client's renders with a running
server's.

### Behind a reverse proxy

The server accepts requests only for its loopback address. Behind a proxy such
as Caddy, pass `--public-origin https://host[:port]`: requests carrying that
Host and Origin are accepted as well, and the per-run token is still required.
The content security policy allows WebAssembly (`'wasm-unsafe-eval'`) and
same-origin workers.

## Rendering and limits

Rendering runs on HTTP worker threads, separate from browser interaction.
The UI retains only the latest pending view and allows one native request at
a time. The server serializes open/render access, bounds its HTTP worker
queue, and checks an image generation on each render. The browser discards
results after view/plane/image changes. An interrupted HTTP request cannot
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
256. Ordinary viewport requests match physical display pixels; exceptionally
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

The server restricts Host/Origin, requires a per-run token for API requests,
and serves only its embedded assets and configured image root. TLS and
optional cpp-httplib compression SDKs are disabled. The pinned header is
cpp-httplib 0.36.0 from the uploaded archive, SHA-256
`0e3802d09922e38012dc25c9408d43473c4a938a4b55592d4c6cff21bbfff586`.

CTest `pyra_web_server` exercises real HTTP routes, RGB8 pixels, RGB16 display
range, bounded overview/render sizes, stale generations, and local access
checks using generated native fixtures. Browser interaction and real-slide
validation are separate manual checks.
