# Pyra C interface

Include `<pyra/pyra.h>` and link `Pyra::pyra`, either from `add_subdirectory`
or an installed `find_package(Pyra CONFIG REQUIRED)` package. The implementation
uses C++20, but the installed interface is valid C. All exported functions use
C linkage. Build outputs are `pyra.dll` plus an import library on Windows and
`libpyra.so` on Linux. Codec runtime dependencies still need deployment.

On Linux, a custom libjpeg-turbo 3 installation must be discoverable when
linking and running consumers; the system's older libjpeg with the same SONAME
cannot substitute for it. For the documented Client-WSL development SDK, use
`export LD_LIBRARY_PATH="$HOME/.local/opt/pyra-jpeg/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"`
before building/running an installed-package consumer. An application package
can instead deploy the compatible codec runtime using its loader configuration.

## Ownership and threads

Create a `pyra_context`, open `pyra_slide` handles in it, close each handle,
then destroy the context. Destroying a context with live handles returns
`PYRA_BUSY` and leaves it usable. Returned channel and property strings are
UTF-8, immutable, borrowed pointers valid until their slide handle closes.
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
sample range. Resolution levels include synthetic coarse levels, when built.
Metadata is available through property count and indexed property queries.

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
Row padding is untouched. The destination is not retained or freed by Pyra,
and may be unaligned because output rows are copied as bytes.

Rows run from top to bottom. RGB8/RGB16 store interleaved R, G, B samples;
Gray8/Gray16 store one integer sample. Integer widths are 8/16 bits, with
16-bit samples in host byte order. Float32 is native IEEE-754 binary32. The
output format must match the selected plane; this interface performs no
display conversion, channel composition, ICC transformation, or range scaling.
For example, 12-bit sensor samples remain 0..4095 in a 16-bit container.
`max_sample_value` is the declared/full integer range, or 1.0 for float display.

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

## Shared cache budgets

Options default to 256 MiB compressed cache, 256 MiB decoded cache, a maximum
64 Mi-pixel output image, and one decoder worker per active call. Both cache
budgets apply across all sources in the context. The compressed budget must
be positive; a zero decoded budget selects direct decoding without retaining
decoded tiles. Temporary decoded tiles needed to build synthetic overview
levels are released before open returns when decoded caching is disabled.
Neither zero means unlimited in this interface. Worker and
image limits must be positive. One context executes only one operation at a
time, so it cannot multiply decoder pools through simultaneous renders.

The context bounds retained tile byte/pixel vector capacity, rather than just
occupied bytes. On pressure it reclaims backing pools of other idle slides,
oldest slide first. The active slide's existing tile LRU handles reuse within
its pools. This first policy evicts a whole cache kind from an idle slide,
rather than maintaining a per-tile global LRU. Independent compressed/decoded
budgets prevent either kind from consuming the other's allowance. A request
whose simultaneously pinned tiles cannot fit can return `PYRA_INCOMPLETE`.

`pyra_context_get_stats` reports allocated capacity, shared sources, live
handles, cross-slide cache-pool reclamations, attempted renders, and incomplete
renders. No cache is allocated per client. Budgets exclude tile bookkeeping,
metadata, auxiliary images, synthetic coarse rasters, codec scratch space,
temporary assembled/resampled rasters, allocator overhead, and caller memory.
Growing a backing vector also temporarily overlaps its old and replacement
allocation. These are cache limits, not hard process-RSS limits. The server
must bound output sizes, request queues, open slides, response caches, and
its own encoding/output memory. Separate contexts and processes need their
budgets divided by the application.

## Minimal C use

```c
#include <pyra/pyra.h>
#include <stdlib.h>

int read_region ( const char *path )
{
	pyra_context *context = NULL;
	pyra_slide *slide = NULL;
	pyra_error error;
	pyra_image_request request = { 0 };
	pyra_image_layout layout = { 0 };
	pyra_image_buffer buffer = { 0 };
	int result = 1;
	if (pyra_context_create( NULL, &context, &error ) != PYRA_OK)
		return result;
	if (pyra_open( context, path, &slide, &error ) != PYRA_OK)
		goto cleanup;
	request.struct_size = sizeof( request );
	request.plane_index = 0;
	request.rectangle = (pyra_rect){ 0, 0, 256, 256 };
	request.mpp = 1;
	layout.struct_size = sizeof( layout );
	if (pyra_get_image_layout( slide, &request, &layout, &error ) != PYRA_OK)
		goto cleanup;
	buffer.struct_size = sizeof( buffer );
	buffer.pixel_format = layout.pixel_format;
	buffer.data = malloc( layout.capacity_bytes );
	buffer.capacity_bytes = layout.capacity_bytes;
	buffer.row_stride_bytes = layout.row_stride_bytes;
	if (buffer.data && pyra_get_image( slide, &request, &buffer, &error ) == PYRA_OK)
		result = 0; /* Consume the pixels here. */
	free( buffer.data );
cleanup:
	if (slide)
		pyra_close( slide, NULL );
	pyra_context_destroy( context, NULL );
	return result;
}
```
