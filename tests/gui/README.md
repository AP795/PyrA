# Linux test viewer and render evaluator

The optional `pyra_gui_test` application opens slides through Pyra's public C
reader interface. All GUI/evaluator sources live here; the library has no Qt
dependency. The viewer supports mouse dragging, wheel zoom, Fit, 1:1, and
selection of a channel/Z plane. The inspector displays the slide's metadata.
The **Open WSI** menu offers **PyrA native** and **OpenSlide**. Its main button
and Ctrl+O reuse the last successfully selected reader. OpenSlide is disabled
unless the library was built with `PYRA_WITH_OPENSLIDE=ON`; see the
[Windows and Linux dependency setup](../../README.md#optional-openslide-backend).
Open WSI includes the file path, reader, TBF/PBF cache sizes in MiB, and decoder
thread count. Browse selects the file. PBF 0 disables retained decoded tiles;
TBF must be at least 1 MiB in this dialog. OpenSlide enables PBF only, since it
does not expose the native TBF or decoding-thread policies. Successful opens
retain the selected settings for subsequent opens in the same viewer session.
The settings initialize the C API context; changing them means reopening the
slide, which retains a compatible recording and resets library caches. Replay
reports show the actual selected settings rather than fixed defaults.

The inspector's minimap shows a cached thumbnail of the selected plane and
the current viewport outline. Click or drag inside the thumbnail to center
the main view without changing its zoom. Presses in the surrounding letterbox
are ignored; an active drag beyond the thumbnail clamps to the slide edge.
The outline updates immediately, while image rendering uses the existing
coalesced worker path. The final release position retains fractional metric
coordinates.

The thumbnail is generated on the worker only when opening a slide or changing
channel/Z, with a maximum edge of 256 pixels. It has its own render cache,
and overview calls are excluded from recording and replay statistics. The
slide's ordinary tile cache is shared, so generating the overview can warm it.

Keyboard: Ctrl+O opens a slide; F fits; 1 selects native resolution; +/- zoom;
arrow keys pan; Escape stops recording or cancels a replay.

## Ubuntu 24.04 / WSL setup

Install the development packages inside your Linux distribution:

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build pkg-config curl nasm \
  qt6-base-dev qt6-wayland libtiff-dev libopenjp2-7-dev
```

Ubuntu 24.04's JPEG package is 2.x; Pyra requires the 12/16-bit APIs from 3.x.
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

An optional first argument opens a slide immediately. Only the loaders enabled
in the Pyra build are available. Ordinary TIFFs are not WSI entry points.

## Configuration and saved status

Normal startup reads these UTF-8 INI files in order:

1. `~/.config/pyra_gui_test/pyra_gui_test.cfg`
2. `~/.config/pyra_gui_test/pyra_gui_test.sts`

The directory follows Qt's user configuration location, so `XDG_CONFIG_HOME`
overrides `~/.config` when set. File locations do not depend on the current
working directory. A missing config is created with:

```ini
[General]
image-path=~/images
```

`image-path` is the Open dialog's initial directory. `~` and `~/...` expand to
the user's home directory. An ordinary relative config path is resolved against
the config directory. Existing user config, including unknown keys/comments,
is left unchanged. INI comments start with `;`.

After a successful image open, normal exit atomically saves its absolute path:

```ini
[General]
last-image=/home/ap/images/sample.ndpi
```

The next startup restores this image automatically. An explicit command-line
image takes precedence. A failed or still-pending open does not replace the
previous successfully loaded image. Missing saved images produce an error in
the viewer while leaving Open available. Malformed config/status is reported
in the viewer and terminal; exit-time status write failures are reported in
the terminal with the affected path.

Missing status starts with an empty viewer. Relative paths in hand-edited
status are resolved against the already-loaded `image-path`; generated status
always uses absolute paths. Normal exit includes closing the window; forced
process termination cannot save status.

`--smoke-test` bypasses user config and status entirely, so generated fixture
paths cannot replace the remembered image. Settings tests use temporary
directories to check defaults, load order, home/relative paths, command-line
priority, Unicode filenames, config preservation, malformed files, and failed
writes.

From Windows PowerShell, enter the user's distribution with:

```powershell
wsl -d Client --cd '~'
```

The configured development source copy is `/home/ap/src/Pyra`; build output is
`/home/ap/build/Pyra`. Run the executable as the regular Linux user so it uses
the WSLg desktop session. Client's restricted `/mnt/c` mount is left intact.
Use Linux paths to slide files copied into Client's accessible filesystem.

## Record / Stop / Replay

1. Choose a reader in **Open WSI**, open a slide and press **Record**. This clears the previous workload and
   requests a frame of the current view.
2. Navigate or change channel/Z. Every actual Pyra render invocation is recorded,
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

Recordings stay in memory for the current slide, including reopening it with
either reader. Opening a different slide clears them. Pressing Record starts a
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
default to a 256 MiB PBF budget; native also defaults to 256 MiB TBF and one
decoder thread. Open WSI can change these values. Context operations serialize;
OpenSlide uses its own decoder policy. Opening, overview generation
and the initial view can warm these caches before replay. Compare repeated runs
in both reader orders; reopening does not flush the OS file cache. Backend
sampling/decoder differences can change pixels even with matching geometry.
