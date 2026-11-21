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
**Generate…** writes a synthetic bright-field z-stack (`pyra_make_synthetic`)
into the folder last shown by **Open image** and then opens it. Its dialog
has the Qt viewer's slide, focus and tissue settings; the request sends
fractions as 0..1 (`z_step`, `focus`, `nucleus`, `spacing`, `positive`,
`tumour`, `lymphocytes`, each optional with the library default). Generation
runs in the background: `POST /api/generate` starts it, `GET /api/generate`
reports tile progress, and `POST /api/generate-cancel` stops it. One
generation runs at a time, and an existing file name is refused.
The existing Qt evaluator remains available for recording/replay.

The combined Linux bundle keeps one shared cpp-httplib copy in its top-level
`httplib/` directory. The viewer defaults to this directory beside the PyrA
source root. When building from a different layout, set `PYRA_HTTPLIB_DIR`
to a directory containing both `httplib.h` and its `LICENSE`. The offline
helper sets this path from the supplied dependency directory automatically.

## Build and run on Linux

With existing codec SDKs available:

```sh
cmake -S . -B out/build/linux-web -DCMAKE_BUILD_TYPE=Release \
  -DPYRA_BUILD_GUI_TEST=OFF -DPYRA_BUILD_WEB_TEST=ON
cmake --build out/build/linux-web --parallel 2
ctest --test-dir out/build/linux-web --output-on-failure
out/build/linux-web/tests/gui/web/pyra_web_test --root /data/images
```

If the SDKs are not discoverable, provide the TIFF, JPEG and OpenJPEG paths
documented in the root README. `PYRA_TIFF_EXTRA_LIBRARIES` supplies additional
link libraries for custom static TIFF builds, such as a static zlib archive.

For the supplied source dependency ZIP, use the offline helper instead:

```sh
mkdir -p /data/dependencies
cd /data/dependencies
cmake -E tar xf /path/to/Pyra_dep_linux.zip
cd /path/to/Pyra
bash tests/gui/web/build_linux.sh /data/dependencies/Pyra_dep_linux /data/pyra-build
/data/pyra-build/install/bin/pyra_web_test --root /data/images
```

The same helper accepts an extracted original `Pyra_dep.zip` that includes
`zlib132`. It builds supplied codec sources in isolation, builds PyrA, runs
CTest, and installs the viewer. GCC 13 or a newer C++20 toolchain, CMake >=3.20,
Make and Bash are required; no Qt, SDL, Python, Node, package installation,
Internet access or system codec development packages are used. JPEG assembly
SIMD is disabled by this minimal dependency adapter. See
[dependency package details](dependencies/README.md) for codec coverage.
The helper enables HTJ2K tiles (`PYR_WITH_OPENJPH`) with the OpenJPH that the
adapter installs. When the package also contains `dcmtk370/` (the DCMTK 3.7.0
modules WSIA builds: ofstd, oflog, dcmdata, dcmimgle, dcmimage, dcmsr, dcmiod,
dcmfg, dcmseg), it builds that subset as static, position-independent libraries
without apps, tests or external libraries, and enables the DICOM loader
(`PYR_WITH_DCMTK`). It prints the selected codecs before configuring PyrA.
Open a DICOM slide through one of its `.dcm` files, not the folder.

The default image directory is `tst_in/` in the checkout used to build the
viewer, independent of its working directory. Use `--root IMAGE_DIRECTORY`
to select another directory.

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
