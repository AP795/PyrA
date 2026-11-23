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

## Rendering and limits

Rendering runs on HTTP worker threads, separate from browser interaction.
The UI retains only the latest pending view and allows one native request at
a time. The server serializes open/render access, bounds its HTTP worker
queue, and checks an image generation on each render. The browser discards
results after view/plane/image changes. An interrupted HTTP request cannot
cancel an already-running native render.

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
