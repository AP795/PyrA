<!--
PyrA Project, Git ap795/PyrA
Copyright (c) 2026 Alexandre Papine
SPDX-License-Identifier: MIT
-->

# Test viewer and render evaluator

The optional `pyra_gui_test` application opens slides through PyrA's public C
reader interface. All GUI/evaluator sources live here; the library has no Qt
dependency. The viewer supports mouse dragging, wheel zoom, Fit, 1:1, and
selection of a channel/Z plane. A vertical slider immediately right of the
minimap selects the current channel's available Z positions and displays Z in
micrometres. It is hidden for single-Z channels and stays synchronized with the
channel/Z selector. Switching Z preserves the view position and zoom.
ZYP stacks expose every stored focal plane and initially select the central
one. Their uncalibrated positions are labelled as plane numbers, without µm.
Replay reports include the rendered width and height, or their ranges when
the recorded calls produce different raster sizes.
The inspector displays the slide's metadata.
The **Open WSI** menu offers **PyrA native** and **OpenSlide**. Its main button
and Ctrl+O reuse the last successfully selected reader. OpenSlide is disabled
unless the library was built with `PYRA_WITH_OPENSLIDE=ON`; see the
[Windows and Linux dependency setup](../../README.md#optional-openslide-backend).
Open WSI includes the file path, reader, Arena/Level cache type, TBF/PBF cache
sizes in MiB, and decoder thread count. Arena shares each cache pool across
levels; Level gives each level a separate pool. Browse selects the file.
PBF 0 disables retained decoded tiles;
TBF must be at least 1 MiB in this dialog. OpenSlide enables PBF and decoder
threads only, since it does not expose the native TBF policy or Arena/Level
selection; its threads read
row bands of each view concurrently. Successful opens
retain the selected settings for subsequent opens in the same viewer session.
The settings initialize the C API context; changing them means reopening the
slide, which retains a compatible recording and resets library caches. Replay
reports show the actual selected settings rather than fixed defaults.
The open report shows thread count and cache type. Replay reports additionally
show TBF and PBF hit percentages, with hits and misses counted only during that
replay. A decoded hit bypasses TBF lookup; synthetic overview pixels and reused
assembled frames bypass tile-cache lookup. Ratios are N/A for no lookups or
OpenSlide's unavailable counters; disabled native PBF is identified explicitly.

The inspector's minimap shows a cached thumbnail of the selected plane and
the current viewport outline. Click or drag inside the thumbnail to center
the main view without changing its zoom. Presses in the surrounding letterbox
are ignored; an active drag beyond the thumbnail clamps to the slide edge.
The outline updates immediately, while image rendering uses the existing
coalesced worker path. The final release position retains fractional metric
coordinates.

Immediately below the minimap, blue TBF and green PBF bars show occupied slots
divided by currently allocated slots, with counts and a percentage. Arena mode
covers the whole pyramid; Level mode shows and labels the actual level used by
the last main-view render, including native fallback selection. Minimap rendering
does not replace that selection. A full bar means all currently allocated slots
are occupied; the pool can still grow within its configured memory budget.
The display refreshes after open, overview, viewport rendering and replay, using
statistics collected on the render worker. No image, no selected level,
unallocated pools, synthetic levels (which have no tile-cache pool), disabled PBF,
and unavailable OpenSlide occupancy are labeled explicitly.
For OpenSlide, the PBF label shows the configured decoded-tile cache capacity
and states that occupancy is unavailable. Normal rendering/replay skips the
native-only slot query, avoiding caught unsupported-operation exceptions.
Every Open initializes fresh TBF/PBF caches, including reopening the same file.
Pyramid opening and the initial overview/view may immediately populate them;
compatible recorded positions are retained independently of cache contents.

The thumbnail is generated on the worker only when opening a slide or changing
channel/Z, with a maximum edge of 256 pixels. It has its own render cache,
and overview calls are excluded from recording and replay statistics. The
slide's ordinary tile cache is shared, so generating the overview can warm it.

Keyboard: Ctrl+O opens a slide; F fits; 1 selects native resolution; +/- zoom;
arrow keys pan; Escape stops recording or cancels a replay.

## Log files

Each viewer run creates a UTF-8 session log named
`pyra-gui-YYYYMMDD-HHMMSS-mmm-PID.log` in the configured PyrA build tree's
`logs/` directory. The local Windows preset writes to
`out/build/windows-release-local/logs/`. Log paths are independent of the
working directory. Logging starts before Qt initializes and captures stdout,
stderr (including native reader DLL diagnostics), Qt messages, WSI open results
and replay reports. Writes are flushed immediately, and the log can be read
while the viewer is running.

The Windows viewer uses the GUI subsystem, so F5 opens the viewer without a
terminal window. No debugger redirection arguments are needed. An unavailable
log directory produces a startup error dialog instead of silently losing logs.

## Ubuntu 24.04 / WSL setup

Install the development packages inside your Linux distribution:

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build pkg-config curl nasm \
  qt6-base-dev qt6-wayland libtiff-dev libopenjp2-7-dev
```

Ubuntu 24.04's JPEG package is 2.x; PyrA requires the 12/16-bit APIs from 3.x.
The helper below downloads libjpeg-turbo 3.1.3 from its official GitHub release,
checks its SHA-256, and builds it under `~/.local/opt/pyra-jpeg`. It uses
JPEG-8 ABI compatibility to match Ubuntu's libtiff and does not replace system
libraries. It needs internet access on its first run.

```sh
bash tests/gui/bootstrap_jpeg.sh
cmake -S . -B "$HOME/build/Pyra" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPYRA_BUILD_GUI_TEST=ON \
  -DJPEG_INCLUDE_DIR="$HOME/.local/opt/pyra-jpeg/include" \
  -DJPEG_LIBRARY="$HOME/.local/opt/pyra-jpeg/lib/libjpeg.so" \
  -DCMAKE_BUILD_RPATH="$HOME/.local/opt/pyra-jpeg/lib"
cmake --build "$HOME/build/Pyra" --parallel 2
ctest --test-dir "$HOME/build/Pyra" --output-on-failure
"$HOME/build/Pyra/tests/gui/pyra_gui_test"
```

An optional first argument opens an image immediately. Only the loaders enabled
in the PyrA build are available.

## TIFF images and IPX file-sets

Ordinary TIFF images open as a gallery. Put them in a directory, then either
run `pyra_make_gallery <directory> [--mpp X]` or, in the browser viewer, open
the directory in **Open image** and press **Make gallery**; then open the
`.glr` manifest it wrote. The Qt viewer opens existing `.glr` files. A legacy
IPX file-set (`.ipx` with `.imd` records) becomes a gallery the same way.

## Synthetic z-stacks

**Generate…** in either viewer writes a synthetic bright-field IHC z-stack and
opens it. You set the image size, tile size, pixel size, compression (JPEG,
JPEG 2000, HTJ2K, JPEG XL; only those this build can encode) and quality or
lossless. Under **Focus** you set the number of Z levels, the plane step and
where in the stack the tissue is sharp; it blurs increasingly away from there.
Under **Tissue** you set the tumour nucleus diameter and spacing, and as
percentages the positive tumour nuclei, the tumour area and the lymphocyte
share of stromal nuclei. See `pyra_make_synthetic` in
[docs/c-api.md](../../docs/c-api.md).
- **Qt viewer:** writes to the path you give (asking before replacing a file), defaulting to
  `synthetic_cells.ptf` in the image directory. The progress bar in the status
  bar tracks the write.
- **Browser viewer:** writes into the folder the **Open image** dialog last
  showed. It refuses an existing name and shows tile progress in the dialog,
  where **Cancel** stops the write.
- **Command line:** `pyra_make_synthetic out.ptf [--size WxH] [--tile N] [--z N]
  [--mpp X] [--codec jpeg|jp2k|htj2k|jxl] [--quality 1..100|lossless]
  [--z-step UM] [--focus 0..1] [--nucleus UM] [--spacing UM] [--positive 0..1]
  [--tumour 0..1] [--lymphocytes 0..1]` does the same; fractions are 0..1 here.

## Configuration and saved status

Normal startup reads these UTF-8 INI files in order:

1. `~/.config/pyra_gui_test/pyra_gui_test.cfg`
2. `~/.config/pyra_gui_test/pyra_gui_test.sts`

The directory follows Qt's user configuration location, so `XDG_CONFIG_HOME`
overrides `~/.config` when set. File locations do not depend on the current
working directory. A missing config is created with:

```ini
[General]
image-path=/path/to/Pyra/tst_in
```

The generated path is the absolute `tst_in` directory in the source checkout used
to build the viewer, independent of the working directory. The previous generated
value `~/images` also selects this default. Other `image-path` values override the
Open dialog's initial directory. `~` and `~/...` expand to
the user's home directory. An ordinary relative config path is resolved against
the config directory. Existing user config, including unknown keys/comments,
is left unchanged. INI comments start with `;`.

After a successful image open, normal exit atomically saves its absolute path
and the current recording in the existing status file:

```ini
[General]
last-image=/home/user/images/sample.ndpi
```

The next startup restores this image automatically and restores compatible
recorded requests, enabling Replay. Rectangles, MPP, channel/Z, order and
duplicates are preserved. Recording does not resume automatically. The status
file stores the recording as a versioned JSON byte array in the `recording` key.
Restoration checks the canonical source path, size, modification time and the
recorded planes' pixel formats and metric geometry. Incompatible or malformed
recordings are reported and skipped while the image remains usable. These
checks do not hash WSI content or all files backing a multifile image.

An explicit command-line image takes precedence and never receives another
image's recording. Normal exit cancels replay, discards undispatched navigation,
and waits for queued open/render/record operations before saving the authoritative
worker state. Closing during an active recording saves it without requiring Stop.
A failed startup with no open image retains the previous saved session. Missing saved images produce an error in
the viewer while leaving Open available. Malformed config/status is reported
in the viewer and session log; exit-time status write failures are reported in
the session log with the affected path.

Missing status starts with an empty viewer. Relative paths in hand-edited
status are resolved against the already-loaded `image-path`; generated status
always uses absolute paths. Normal exit includes closing the window; forced
process termination cannot save status.

`--smoke-test` bypasses user config and status entirely, so generated fixture
paths cannot replace the remembered image. Settings tests use temporary
directories to check defaults, load order, home/relative paths, command-line
priority, Unicode filenames, config preservation, malformed files, and failed
writes.

From Windows PowerShell, enter your WSL distribution with:

```powershell
wsl -d <distribution> --cd '~'
```

Run the executable as the regular Linux user so it uses the WSLg desktop
session. Open slides through Linux paths; if the distribution restricts its
`/mnt/c` mount, copy them into its own filesystem first.

## Record / Stop / Replay

1. Choose a reader in **Open WSI**, open a slide and press **Record**. This clears the previous workload and
   requests a frame of the current view.
2. Navigate or change channel/Z. Every actual PyrA render invocation is recorded,
   including repeated identical requests. A request retains metric `x, y, w, h`
   in microns, sampling MPP, and channel/Z. Output dimensions are derived from
   the rectangle and MPP by the C interface.
   Preview painting and coalesced navigation events do not invoke the renderer.
3. Press **Stop**. Its position in the serialized worker queue defines the
   recording boundary. Already running/queued calls precede Stop; undispatched
   navigation requests are discarded.
4. Press **Replay**. Requests execute sequentially, without coalescing, sleeping,
   changing their rectangles, or substituting the current window size. The GUI
   remains responsive. Stop cancels between calls, after any running call ends.
5. Choose the other reader in **Open WSI** and reopen the **same file**. The
   recording is retained if its planes, RGB format and metric geometry match.
   Press **Replay** again. Both reports remain in the inspector and identify
   their backend/version and separate opening time. A failed or incompatible
   open leaves the current slide and workload usable.

The report includes recorded/attempted counts, failures, incomplete frames,
total render time, mean, median, nearest-rank p95, minimum/maximum, and render
throughput. All attempts contribute to timing statistics; failures/incomplete
frames are reported separately. Wall time is shown separately.
Replay completion reveals the inspector and scrolls to the new report, including
when the inspector was hidden or the report pane was collapsed. Earlier replay
reports remain available. The animated busy indicator is hidden during replay;
the status bar and Stop action still show that replay is active.

Render time measures the synchronous `pyra_get_image` call, including tile
loading, decoding, caching, resampling, and copying pixels to the caller's
buffer. It excludes caller buffer allocation, Qt image conversion, painting,
interaction delays, layout queries, and slide opening. Requests contain a
plane, metric rectangle, and mpp; raster dimensions follow the C interface's
rounding rule. Replay retains the existing slide and tile-cache state, so it
is **not a cold-cache benchmark** and does not flush the OS
page cache. Replays are a workload evaluator, not evidence of a speedup without
a comparable baseline.

Recordings belong to the current slide, including reopening it with
either reader, and survive normal application exit/startup through the per-user
status file. Opening a different slide clears them. Pressing Record starts a
new workload and clears the report. The viewer writes no recording/configuration sidecars beside
slide files. The C wrapper uses request-local assembled rasters. The GUI retains
one prepared display image, extending each view by up to a factor of 1.5 on each
axis within its existing 4096-pixel edge limit. Panning reuses that image while
coverage, channel/Z, and mpp match; crossing its edge schedules fresh rendering.
Navigation is coalesced without waiting for mouse motion to stop. At most one
render is active and one latest view is pending. These caller-owned display
images are outside the C context's tile-cache caps.
If the expanded request is incomplete, the viewer retries the exact visible
rectangle and disables overscan at that resolution for the current plane.

Recordings contain actual C render calls, including their prepared rectangles;
pans served entirely by the existing image do not add a call. Full-range RGB8
and Gray8 display uses native packed Qt images; wider/narrow-range samples keep
their explicit display scaling. Unsupported compression appears in the status
bar and inspector with its codec/build requirement.

The GUI links only the shared C reader library. Its synthetic PTF writer runs in the
separate `pyra_fixture_writer` executable beside the GUI and evaluator tests.

## Automated checks and GUI smoke mode

The evaluator tests use synthetic multi-plane images to verify exact request
order/duplicates, sizes, plane selection, recording boundaries, restart/reset,
cancellation, grayscale display range, and known timing statistics.

CTest also runs an offscreen GUI smoke test that opens a synthetic slide,
operates Record/pan/zoom/Stop/Replay, and saves a screenshot in the build tree's
`tests/gui/test_out/gui/` directory. It does not require a desktop session.
The workflow also clicks/drags the minimap, checks the exact final center and
preserved zoom, and switches focal planes to exercise thumbnail replacement.
Dedicated minimap tests cover wide/tall letterboxing, nonzero/negative origins,
fractional coordinates at large slide extents, viewport outlines, ignored
letterbox presses, and cancelled drag state. Evaluator tests verify that
thumbnail calls do not enter recordings or replay.

To test the actual WSLg window with the same automated workflow:

```sh
QT_QPA_PLATFORM=xcb "$HOME/build/Pyra/tests/gui/pyra_gui_test" --smoke-test \
  "$HOME/build/Pyra/tests/gui/test_out/wslg"
```

The window closes after the checks and screenshot. Synthetic tests do not
establish performance on real SVS/NDPI/DICOM/CZI slides.

OpenSlide exposes one unnamed RGB8 brightfield plane; native channel/Z stacks
and deep grayscale planes are not interchangeable workloads. Both readers
default to a 32 MiB PBF budget and four decoder threads; native also defaults to
32 MiB TBF. Open WSI can change these values. Context operations serialize;
within one call, native threads decode tiles and OpenSlide threads read row
bands. Opening, overview generation
and the initial view can warm these caches before replay. Compare repeated runs
in both reader orders; reopening does not flush the OS file cache. Backend
sampling/decoder differences can change pixels even with matching geometry.
