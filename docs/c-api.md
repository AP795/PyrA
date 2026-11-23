<!--
PyrA Project, Git ap795/PyrA
Copyright (c) 2026 Alexandre Papine
SPDX-License-Identifier: MIT
-->

# PyrA C interface

Include `<pyra/pyra.h>` and link `Pyra::pyra`, either from `add_subdirectory`
or an installed `find_package(Pyra CONFIG REQUIRED)` package. The implementation
uses C++20, but the installed interface is valid C. All exported functions use
C linkage. Build outputs are `pyra.dll` plus an import library on Windows and
`libpyra.so` on Linux. Codec runtime dependencies still need deployment.

On Linux, a custom libjpeg-turbo 3 installation must be discoverable when
linking and running consumers; the system's older libjpeg with the same SONAME
cannot substitute for it. For the SDK built by the documented Linux setup, use
`export LD_LIBRARY_PATH="$HOME/.local/opt/pyra-jpeg/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"`
before building/running an installed-package consumer. An application package
can instead deploy the compatible codec runtime using its loader configuration.

## Ownership and threads

Create a `pyra_context`, open `pyra_wsi` handles in it, close each handle,
then destroy the context. Destroying a context with live handles returns
`PYRA_BUSY` and leaves it usable. Returned channel and property strings are
UTF-8, immutable, borrowed pointers valid until their WSI handle closes.
Paths supplied to `pyra_open` are UTF-8. Source files must not change while
open; reopening a modified source requires closing all handles first.

Repeated opens of the same canonical path share internal loader and tile
storage within a context. Each successful open returns its own handle;
closing one leaves the others usable. The context does not retain a source
after its last handle closes. Canonical paths resolve symlinks; case aliases
and hard links are not guaranteed to deduplicate.

Operations on live handles are safe from multiple threads and serialized
within each context in this first version. Decoder workers can run in
parallel inside one render. Independent contexts can operate concurrently,
with independent budgets. A handle's close/destroy must not run concurrently
with any use of that handle. A context's destroy must not race any context
operation. Callers must provide separate destination and error storage for
concurrent calls. No global process singleton is required.

## Reader backends

ABI version 2 adds `pyra_context_options.backend`. Initialize options with
`pyra_context_options_init`, then choose `PYRA_BACKEND_NATIVE` (default) or
`PYRA_BACKEND_OPENSLIDE` before creating the context. A context uses one backend
for all of its sources. Query `pyra_get_backend_info` for its display name,
version and build availability; returned strings have static/library lifetime.
An unknown backend returns `PYRA_INVALID_ARGUMENT`. Selecting OpenSlide in a
build without `PYRA_WITH_OPENSLIDE=ON` returns `PYRA_UNSUPPORTED` and a null
context. Consumers built against earlier ABI versions must rebuild; the current
ABI version and shared-library SOVERSION are 4. ABI 4 adds tile-cache hit/miss
counters to `pyra_context_stats`.

OpenSlide exposes its main image as one unnamed RGB8 plane at Z=0. It does not
provide PyrA's native channel/Z stacks or original deep sample formats. Source
recognition follows OpenSlide, including generic tiled `.tif` files. Unsupported
sources never fall back to native. Raw OpenSlide properties remain available;
`openslide.*` properties also have `pyra.*` aliases. Backend name/version and
whether the scalar MPP used the 1 um fallback are included as properties.

Rendering uses the same layout validation, centre sampling, output limits,
stride handling and failure atomicity as native. The adapter selects an
OpenSlide pyramid level, reads bounded regions, samples the requested metric
rectangle and converts premultiplied ARGB to RGB on black. No ICC transform is
applied. Staging blocks are bounded to 2048 x 2048 pixels even for a single-level
WSI at coarse resolution; assembled output and coordinate arrays are temporary
memory outside cache budgets. Sources above the renderer's signed-int dimension
range are rejected. As with native, scalar X MPP governs both rendering axes;
Y MPP is preserved as metadata. Missing X MPP uses 1 um/pixel and is identified
by `pyra.mpp-fallback=true`.

OpenSlide I/O/decode errors are terminal for that open source and return
`PYRA_IO_ERROR`, leaving the caller's buffer untouched. Close all shared handles
and reopen to recover. OpenSlide does not expose native-style partial-tile
failure accounting: transparent areas composite on zero, and a fatal region
failure does not return a partial image.

The pinned official binary build 4.0.1.2 bundles libtiff 4.7.1. During validation,
a valid uncompressed RGB TIFF with 16 x 16 tiles failed inside that upstream
library with `Invalid tile byte count ... Expected 768, got 1024`. The adapter
surfaces this as an I/O error. Staging/alpha regression fixtures use lossless
LZW; common-reader comparison fixtures use complete JPEG tile streams.

One OpenSlide decoded tile cache is shared by every source in a context and
uses `decoded_cache_bytes`; zero disables retention. `compressed_cache_bytes`
has no effect on OpenSlide, although common option validation still requires
a positive value. OpenSlide itself decodes on the calling thread, so the
adapter cuts each `pyra_get_image` into row bands and reads up to
`decode_threads` of them concurrently through the same slide handle and
cache. Bands are fixed slices of 128 level rows, rounded up to whole tile
rows, so the image is identical for every `decode_threads` value and no tile
row is decoded twice. Compressed cache usage is zero. Decoded cache usage (unless its
budget is zero) and eviction counts are `PYRA_STAT_UNAVAILABLE`, because upstream
does not expose those counters. Other context counters keep their usual meaning.

For comparisons, use identical slide bytes, metric requests and compatible
RGB8 planes. The GUI checks recorded plane identity, format, bounds and native
MPP before retaining a recording across readers. Pyramid contents, level
selection, decoder implementations and alpha handling can still produce pixel
differences. GUI replay timings include all work inside `pyra_get_image`,
including the OpenSlide adapter, but exclude opening, caller allocation, Qt
conversion and painting. Opening time is reported separately. Replays retain
caches; reopening creates a new library context without flushing OS caches.

## Structures and errors

Call `pyra_context_options_init` before changing options. Set `struct_size`
to `sizeof(structure)` on requests, output structures, and buffer descriptors.
The context options carry `PYRA_ABI_VERSION`; incompatible versions/sizes are
rejected. Structures use the platform's default packing and native pointer
width; do not apply a packing pragma around the header. Binary compatibility
is for the same architecture and platform ABI.

Functions return fixed-width `pyra_status` values. An optional caller-owned
`pyra_error` receives a terminated diagnostic, truncated to its capacity;
successful calls clear it. C++ exceptions are caught by the exported interface.
Null arguments, invalid plane/property indices, invalid geometry, incompatible
structures, mismatched pixel formats, and invalid buffer layouts are rejected.
Creation/open failures set their handle output to null. Other output values
are usable only after success, except image pixels returned on `PYRA_INCOMPLETE`.

## Geometry and pixel storage

Enumerate planes with `pyra_get_plane_count` and `pyra_get_plane_info`. Plane
indices are local to an open slide. Information includes channel, Z in
micrometres, native bounds and rendering mpp, level count, pixel format, and
sample range. `z_um` is NaN when the physical Z coordinate is unknown, as for
ZYP focal-plane identifiers. Such planes remain individually selectable by
`plane_index`; do not interpret their indices as micrometres. Native ZYP
metadata records `pyra.z-unit=index` and `pyra.default-plane-index` (the central
plane's enumeration index). Resolution levels include synthetic coarse levels, when built.
Metadata is available through property count and indexed property queries.
Normalized metadata uses the `pyra.` namespace, including `pyra.vendor`,
`pyra.mpp-x`, `pyra.mpp-y`, `pyra.objective-power`, `pyra.level-count`,
`pyra.level[i].*`, `pyra.associated[name].*`, and `pyra.icc-size` when available.
Raw metadata retains its format-specific namespace, such as `tiff.` or `aperio.`.

An image request specifies the plane index, rectangle `(x, y, width, height)`
in micrometres, and positive mpp. Rectangle edges are half-open. Output width
and height are `ceil(extent / mpp)`, snapping a positive ratio to its nearest
integer when the difference is at most `1e-9 * max(1, ratio)`. The entire
rectangle maps onto that raster; edge rounding can slightly change its actual
sampling interval. Layout and rendering use the same calculation. The current
core uses float metric coordinates internally; geometry that loses an entire
extent or cannot be represented is rejected. Source X/Y physical pixel sizes
are reported as metadata when known (zero otherwise), while rendering still
uses the scalar native mpp on both axes.

Query `pyra_get_image_layout` before allocating the destination. It returns
dimensions, pixel format, packed row stride and packed capacity. Reuse a
larger buffer if desired. `pyra_get_image` accepts a stride at least as large
as the packed row size and capacity covering
`(height - 1) * stride + packed_row_bytes`; arithmetic is checked for overflow.
Row padding is untouched. The destination is not retained or freed by PyrA,
and may be unaligned because output rows are copied as bytes.

Rows run from top to bottom. RGB8/RGB16 store interleaved R, G, B samples;
Gray8/Gray16 store one integer sample. Integer widths are 8/16 bits, with
16-bit samples in host byte order. Float32 is native IEEE-754 binary32. The
output format must match the selected plane; this interface performs no
display conversion, channel composition, ICC transformation, or range scaling.
For example, 12-bit sensor samples remain 0..4095 in a 16-bit container.
`max_sample_value` is the declared/full integer range, or 1.0 for float display.

For native JPEG tiles without JFIF/Adobe colour markers, the TIFF photometric
declaration supplies RGB versus YCbCr interpretation. This avoids treating an
RGB SVS tile with ambiguous JPEG component IDs as YCbCr. Explicit JPEG colour
markers retain their normal interpretation, including the self-describing
JPEG streams written inside PTF. The hint is applied before libjpeg starts
decompression, for both retained decoded caching and direct decoding.

Pixels outside the plane are zero. When tile loading/decoding fails, available
pixels and zero-filled missing areas are returned with `PYRA_INCOMPLETE`.
Callers doing measurements should reject that status. Other failed image calls
leave the caller's destination untouched. Rendering is synchronous; scheduling,
HTTP, response encoding, and remote client state belong to the application.

Known compression without an enabled decoder returns `PYRA_UNSUPPORTED`, with
the codec and build requirement in the diagnostic. JPEG XL and JPEG 2000 TIFF
compression tags are checked while opening; codecs discovered from tile payloads are checked
while rendering. An unsupported render leaves the destination unchanged.
Ordinary I/O or corrupt-payload failures retain the incomplete-image contract.

## Ordinary TIFF images and IPX file-sets: galleries

`pyra_open` does not open a bare `.tif`, `.tiff` or `.ipx`. A directory of them
becomes one slide through a gallery manifest: `pyra_make_gallery( directory,
mpp, manifest, capacity, error )` lays the directory out and writes
`{directory}/{directory name}.glr` (`{directory}/{slide}.glr` for an IPX
file-set), whose path it returns; `pyra_open` then opens the manifest.

Members are the single-page TIFFs directly in the directory, in natural name
order, each in its original unsigned Gray8, Gray16, RGB8 or RGB16 format; one
pixel type per gallery (the majority's). Multi-page stacks, signed or
floating-point samples and unreadable files are refused and named in the error.
A member's pixel size comes from an `.imd` beside it, then from microscope TIFF
resolution tags, then from `mpp`; members that disagree refuse the gallery, and a
gallery stating no size at all is refused with `PYRA_INVALID_ARGUMENT` until
`mpp` is given. `gallery.mpp-source` records which (`imd`, `tiff`, `user`). A
16-bit gallery's full scale is measured from its samples (`max_sample_value`).

Each member is one tile on a grid with gutters, decoded whole; reduced TIFF
pages and `.ith` thumbnails serve the overview when every member has one, and
the synthetic coarse-level builder covers zoomed-out rendering otherwise. A
compressed cache must hold a member file, and a nonzero decoded cache a decoded
member.

## Synthetic z-stack slides

`pyra_make_synthetic( options, path, error )` writes a synthetic bright-field
IHC slide as a PTF z-stack, for testing viewers and pipelines without shipping
a real slide. Start from `pyra_synthetic_options_init` (4096 × 4096 pixels,
256-pixel tiles, 4 focal planes 1 µm apart in focus at 5/12 of the stack,
0.5 µm/px, JPEG quality 90, 8 µm tumour nuclei 12 µm apart, 30 % positive,
40 % tumour area, 40 % lymphocytes in stroma, a fixed seed) and change any of:

| Field | Meaning | Limits |
|---|---|---|
| `width`, `height` | base level, pixels | 1 .. `PYRA_SYNTHETIC_EDGE_MAX` |
| `tile_edge` | square tile edge, pixels | `PYRA_SYNTHETIC_TILE_MIN` .. `_MAX`, multiple of `_STEP` |
| `z_levels` | focal planes, from z = 0 | 1 .. `PYRA_SYNTHETIC_Z_MAX` |
| `z_step_um` | distance between focal planes, µm | `PYRA_SYNTHETIC_Z_STEP_MIN_UM` .. `_MAX_UM` |
| `focus_fraction` | in-focus depth: 0 = first plane, 1 = last | 0 .. 1 |
| `mpp` | microns per base pixel | `PYRA_SYNTHETIC_MPP_MIN` .. `_MAX` |
| `nucleus_diameter_um` | mean equivalent diameter 2·√(ab) of tumour nuclei | `PYRA_SYNTHETIC_NUCLEUS_MIN_UM` .. `_MAX_UM` |
| `nucleus_spacing_um` | √(area per nucleus) inside tumour nests | `PYRA_SYNTHETIC_SPACING_MIN_UM` .. `_MAX_UM` |
| `positive_fraction` | expected share of DAB-positive tumour nuclei | 0 .. 1 |
| `tumour_fraction` | expected share of the tissue area in tumour nests | 0 .. 1 |
| `lymphocyte_fraction` | share of stromal nuclei that are lymphocytes | 0 .. 1 |
| `codec` | `PYRA_CODEC_JPEG`, `_JPEG2000`, `_HTJ2K`, `_JPEGXL` | `pyra_codec_can_encode` |
| `quality` | 1..100 lossy, `PYRA_QUALITY_LOSSLESS` (not JPEG) | |
| `seed` | same options and seed give the same pixels | |
| `progress`, `progress_user` | called after each tile; return 0 to cancel | |

The slide has white margins around tissue. The tissue holds pale blue
(hematoxylin) tumour nuclei in nests, ±18 % around the requested diameter,
with DAB-positive brown nuclei clustered among them. The clustering never
shifts the mean: the positive rate is the requested fraction modulated by a
field symmetric about it. Stroma holds elongated fibroblast nuclei following
a flow field, small dark lymphocytes (about 5 µm) and faint fibres; 4 % of
fibroblasts and 8 % of lymphocytes are positive whatever `positive_fraction`
says. Stromal nuclei are sparser than tumour nuclei, filling 0.4 rather than
0.85 of the grid that `nucleus_spacing_um` sets. Cell sizes are in microns,
so `mpp` changes how large they look, not how large they are. Each
plane is a full RGB8 level ladder, halving until the slide fits one tile. Its
PTF block states `zlevel` and `mpp`.

The tissue is in focus at `focus_fraction` of the stack height. Each cell
sits at a slightly different depth, and its blur grows by 1.4 µm per µm of
distance from the plane, up to 6 µm. By default a 4-plane stack is sharp at
z = 1, slightly blurred at z = 2 and blurred at z = 0 and z = 3. Every pixel is computed from its position alone, so memory
stays bounded by the tiles being written whatever the size. Tiles are rendered
on all cores. On a 20-thread machine the default slide takes about 7 s.

Invalid options return `PYRA_INVALID_ARGUMENT` naming the field. A codec this
build cannot encode returns `PYRA_UNSUPPORTED` naming its build option. A
cancelled write returns `PYRA_INCOMPLETE`. Neither leaves a file. An existing
file is replaced. The `pyra_make_synthetic` tool and both test viewers'
**Generate** dialogs use this function for PTF.

`pyra_make_synthetic_dicom( options, path, error )` writes the same synthetic
tissue as a DICOM Whole Slide Microscopy pyramid. The path names the `.dcm`
entry file, and lower resolutions go in the sibling `_<stem>_` directory.
Each instance holds all requested focal planes at their specified depths.
`pyra_synthetic_dicom_can_encode( codec )` reports whether the build has
DCMTK and the selected encoder. DICOM generation supports baseline JPEG,
JPEG 2000 and HTJ2K. The progress callback can also run between tiles while
writing metadata or publishing. Existing output is refused.

`pyra_synthetic_count_nuclei( options, x, y, width, height, nuclei, error )`
is the slide's ground truth. It counts the nuclei whose centre lies in a base
pixel rectangle: tumour, positive tumour, fibroblast and lymphocyte, plus the
mean tumour diameter. It renders nothing, so it is fast even for a whole
slide, and it ignores `codec` and `quality`. Use it to check nucleus counts,
positivity indices or tumour/stroma splits that an analysis reports from the
written pixels.

## Shared cache budgets

The existing `pyra_context_options` controls TBF, PBF and native decoding:

| Control | C API field | Meaning |
| --- | --- | --- |
| TBF | `compressed_cache_bytes` | Context-wide retained compressed tile bytes; positive |
| PBF | `decoded_cache_bytes` | Context-wide retained decoded tile bytes; zero disables retention |
| Layout | `cache_topology` | `PYRA_CACHE_ARENA` (default): one pool per cache for the slide, reusable at any level; `PYRA_CACHE_PER_LEVEL`: one pool per level, sized to its tiles |
| Decoding | `decode_threads` | Maximum decoding participants per active call, including the calling thread: native tile decoders, or OpenSlide row-band readers; positive |

Set these fields after `pyra_context_options_init` and before
`pyra_context_create`; they are creation-time settings. For example:

```c
pyra_context_options options;
pyra_context_options_init( &options );
options.compressed_cache_bytes = UINT64_C(128) * 1024 * 1024;
options.decoded_cache_bytes = UINT64_C(512) * 1024 * 1024;
options.decode_threads = 8;
/* Pass &options to pyra_context_create. */
```

OpenSlide uses the decoded/PBF budget and `decode_threads`; its upstream API
does not expose the native TBF policy.

Options default to 32 MiB compressed cache, 32 MiB decoded cache, a maximum
64 Mi-pixel output image, and four decoder workers per active call. Both cache
budgets apply across all sources in the context. The compressed budget must
be positive; a zero decoded budget selects direct decoding without retaining
decoded tiles. Temporary decoded tiles needed to build synthetic overview
levels are released before open returns when decoded caching is disabled.
Neither zero means unlimited in this interface. Worker and
image limits must be positive. One context executes only one operation at a
time, so it cannot multiply decoder pools through simultaneous renders.

Enabled TBF/PBF entries and their backing allocations survive overview
construction and rendering. Returning a reusable slot does not shrink its
allocation. Cache pressure triggers eviction and, when another level or slide
needs the capacity, reclaims unused backing storage. Closing the last handle
to a source releases its pools. Disabled decoded caching retains no PBF scratch
after opening, as described above.

The context bounds retained tile byte/pixel vector capacity, rather than just
occupied bytes. On pressure it reclaims backing pools of other idle slides,
oldest slide first. The active slide's existing tile LRU handles reuse within
its pools. This first policy evicts a whole cache kind from an idle slide,
rather than maintaining a per-tile global LRU. Independent compressed/decoded
budgets prevent either kind from consuming the other's allowance. A request
whose simultaneously pinned tiles cannot fit can return `PYRA_INCOMPLETE`.

`pyra_context_get_stats` reports allocated capacity, shared sources, live
handles, cross-slide cache-pool reclamations, attempted renders, and incomplete
renders. It also reports cumulative `compressed_cache_hits/misses` and
`decoded_cache_hits/misses` for the context, retained after a source closes.
The hit ratio is hits / (hits + misses); no lookups means no ratio. Decoded hits
bypass compressed lookups. Disabled decoded caching, synthetic rasters, sparse
absent tiles and reusable assembled frames do not cause decoded-cache lookups;
synthetic/absent tiles and reusable frames also bypass compressed lookups.
Opening/overview work counts, so subtract snapshots to measure only a replay.
OpenSlide returns `PYRA_STAT_UNAVAILABLE` for all four counters.

`pyra_get_cache_usage(wsi, level_index, &usage, &error)` reports occupied and
currently allocated TBF/PBF slot counts for one WSI. Initialize `usage.struct_size`.
The query returns the topology and actual zero-based native render level index.
Pass -1 for the last render's level, or a previously returned index to query that
level explicitly. Arena mode always counts the whole WSI's arena; Level mode
counts only the selected level's pools. The denominator is allocated slots,
including free reusable slots, rather than the configured maximum memory budget.
Before rendering, Level mode has level_index -1 and no selected pool. Synthetic
levels own their pixels and have no tile-cache pools; `synthetic_level` identifies
them. OpenSlide returns `PYRA_UNSUPPORTED`. The query uses pool counters rather
than walking tiles and is serialized with other operations on the context.

No cache is allocated per client. Budgets exclude tile bookkeeping,
metadata, auxiliary images, synthetic coarse rasters, codec scratch space,
temporary assembled/resampled rasters, allocator overhead, and caller memory.
Growing a backing vector also temporarily overlaps its old and replacement
allocation. These are cache limits, not hard process-RSS limits. The server
must bound output sizes, request queues, open slides, response caches, and
its own encoding/output memory. Separate contexts and processes need their
budgets divided by the application.

## Command-line C++ sample

With `BUILD_TESTING=ON`, build target `pyra_c_api_sample`. Its source is
[`Tests/c_api_sample.cpp`](../Tests/c_api_sample.cpp) and it uses only the public
C reader API plus libjpeg for output, without Qt or internal PyrA headers:

```sh
pyra_c_api_sample input.svs output.jpg
```

The optional output path defaults to `centre.jpg` in the working directory.
The sample opens the native backend, selects plane 0, and renders a centred
1024 x 1024 crop at that plane's `native_mpp`. Pixels outside a smaller slide
are black. JPEG quality is 95. This getting-started example accepts RGB8 planes
and writes their pixels directly, without display conversion. Other formats
are rejected with a clear message. Incomplete renders and other errors return a
nonzero exit code. The separate JPEG helper uses libjpeg's default fatal-error
handler, which reports codec errors and terminates the command-line program.

## Minimal C use

```c
#include <pyra/pyra.h>
#include <stdlib.h>

int read_region ( const char *path )
{
	pyra_context *context = NULL;
	pyra_wsi *wsi = NULL;
	pyra_error error;
	pyra_image_request request = { 0 };
	pyra_image_layout layout = { 0 };
	pyra_image_buffer buffer = { 0 };
	int result = 1;
	if (pyra_context_create( NULL, &context, &error ) != PYRA_OK)
		return result;
	if (pyra_open( context, path, &wsi, &error ) != PYRA_OK)
		goto cleanup;
	request.struct_size = sizeof( request );
	request.plane_index = 0;
	request.rectangle = (pyra_rect){ 0, 0, 256, 256 };
	request.mpp = 1;
	layout.struct_size = sizeof( layout );
	if (pyra_get_image_layout( wsi, &request, &layout, &error ) != PYRA_OK)
		goto cleanup;
	buffer.struct_size = sizeof( buffer );
	buffer.pixel_format = layout.pixel_format;
	buffer.data = malloc( layout.capacity_bytes );
	buffer.capacity_bytes = layout.capacity_bytes;
	buffer.row_stride_bytes = layout.row_stride_bytes;
	if (buffer.data && pyra_get_image( wsi, &request, &buffer, &error ) == PYRA_OK)
		result = 0; /* Consume the pixels here. */
	free( buffer.data );
cleanup:
	if (wsi)
		pyra_close( wsi, NULL );
	pyra_context_destroy( context, NULL );
	return result;
}
```
