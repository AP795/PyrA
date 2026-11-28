// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && !defined(PYRA_STATIC)
#if defined(PYRA_BUILDING_LIBRARY)
#define PYRA_API __declspec(dllexport)
#else
#define PYRA_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define PYRA_API __attribute__((visibility("default")))
#else
#define PYRA_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PYRA_ABI_VERSION 4u
#define PYRA_ERROR_MESSAGE_SIZE 512u
#define PYRA_STAT_UNAVAILABLE UINT64_MAX

typedef struct pyra_context pyra_context;
typedef struct pyra_wsi pyra_wsi;
typedef int32_t pyra_status;
typedef uint32_t pyra_pixel_format;
typedef uint32_t pyra_backend;
typedef uint32_t pyra_cache_topology;

enum {
	PYRA_BACKEND_NATIVE = 0,
	PYRA_BACKEND_OPENSLIDE = 1
};

// How the native reader lays out its tile caches. ARENA: one pool per cache for
// the whole slide, reusable at any resolution. PER_LEVEL: one pool per level,
// each sized to that level's tiles. Applies to slides opened afterwards.
enum {
	PYRA_CACHE_ARENA = 0,
	PYRA_CACHE_PER_LEVEL = 1
};

// The tile caches pyra_clear_cache empties, combined with |.
enum {
	PYRA_CACHE_COMPRESSED = 1,
	PYRA_CACHE_DECODED = 2
};

typedef struct pyra_backend_info {
	uint32_t struct_size;
	uint32_t available;
	const char *name;
	const char *version;
} pyra_backend_info;

enum {
	PYRA_OK = 0,
	PYRA_INVALID_ARGUMENT = 1,
	PYRA_IO_ERROR = 2,
	PYRA_UNSUPPORTED = 3,
	PYRA_OUT_OF_MEMORY = 4,
	PYRA_BUFFER_TOO_SMALL = 5,
	PYRA_INCOMPLETE = 6,
	PYRA_BUSY = 7,
	PYRA_LIMIT_EXCEEDED = 8,
	PYRA_INTERNAL_ERROR = 9
};

enum {
	PYRA_PIXEL_RGB8 = 1,
	PYRA_PIXEL_RGB16 = 2,
	PYRA_PIXEL_GRAY8 = 3,
	PYRA_PIXEL_GRAY16 = 4,
	PYRA_PIXEL_FLOAT32 = 5
};

typedef struct pyra_error {
	char message[PYRA_ERROR_MESSAGE_SIZE];
} pyra_error;

typedef struct pyra_context_options {
	uint32_t struct_size;
	uint32_t abi_version;
	uint64_t compressed_cache_bytes;
	uint64_t decoded_cache_bytes;
	uint64_t max_image_pixels;
	uint32_t decode_threads;
	pyra_backend backend;
	pyra_cache_topology cache_topology;
} pyra_context_options;

typedef struct pyra_rect {
	double x_um;
	double y_um;
	double width_um;
	double height_um;
} pyra_rect;

typedef struct pyra_plane_info {
	uint32_t struct_size;
	pyra_pixel_format pixel_format;
	uint32_t level_count;
	const char *channel;
// NaN when physical Z is unknown (e.g. ZYP). Select by plane_index regardless.
	double z_um;
	pyra_rect bounds;
	double native_mpp;
	double source_mpp_x;
	double source_mpp_y;
	double max_sample_value;
} pyra_plane_info;

typedef struct pyra_property {
	const char *key;
	const char *value;
} pyra_property;

typedef struct pyra_image_request {
	uint32_t struct_size;
	uint32_t plane_index;
	pyra_rect rectangle;
	double mpp;
} pyra_image_request;

typedef struct pyra_image_layout {
	uint32_t struct_size;
	pyra_pixel_format pixel_format;
	uint32_t width;
	uint32_t height;
	size_t row_stride_bytes;
	size_t capacity_bytes;
} pyra_image_layout;

typedef struct pyra_image_buffer {
	uint32_t struct_size;
	pyra_pixel_format pixel_format;
	void *data;
	size_t capacity_bytes;
	size_t row_stride_bytes;
} pyra_image_buffer;

typedef struct pyra_context_stats {
	uint32_t struct_size;
	uint64_t compressed_cache_bytes;
	uint64_t decoded_cache_bytes;
	uint64_t cache_pool_evictions;
	uint64_t shared_wsis;
	uint64_t wsi_handles;
	uint64_t render_calls;
	uint64_t incomplete_renders;
// Cumulative tile lookups across sources, including opening/overview work.
// A hit is hits / (hits + misses). Ready decoded pixels bypass TBF lookup;
// synthetic/absent tiles and reusable assembled frames do not cause lookups.
// Disabled PBF has zero lookups. OpenSlide returns PYRA_STAT_UNAVAILABLE.
	uint64_t compressed_cache_hits;
	uint64_t compressed_cache_misses;
	uint64_t decoded_cache_hits;
	uint64_t decoded_cache_misses;
// Encoded-tile reads (pyra_read_encoded_tile) that found no compressed-cache
// slot and read the file past the cache; each is also counted as a miss.
// Non-zero means the compressed budget is too small for the workflow.
	uint64_t compressed_cache_bypasses;
// Stored tiles read from the source - the file, or for a remote slide the bytes
// put into it - whether into the compressed cache or past it. A hit reads
// nothing; sparse tiles and synthetic levels are not counted.
	uint64_t source_tile_reads;
// Bytes of the cache entries holding a tile: what each cache holds, in either
// topology. A compressed entry is exactly its tile's bytes, and the compressed
// cache allocates nothing ahead, so compressed_cache_used_bytes equals
// compressed_cache_bytes. Decoded slots are fixed: decoded_cache_bytes counts
// the storage allocated, which pools grow many slots at a time and keep.
	uint64_t compressed_cache_used_bytes;
	uint64_t decoded_cache_used_bytes;
// Tiles each cache holds. compressed used_bytes / tiles is the average tile's
// encoded size; decoded used_bytes / tiles is the decoded slot size.
	uint64_t compressed_cache_tiles;
	uint64_t decoded_cache_tiles;
} pyra_context_stats;

// Occupancy of the open WSI's shared arena or one level's pools. Counts cover
// allocated slots (including reusable free slots), not maximum budget capacity.
// A free compressed slot holds no bytes - compressed entries are allocated at
// their tile's size - so for that cache only the used count measures storage.
typedef struct pyra_cache_usage {
	uint32_t struct_size;
	pyra_cache_topology topology;
	int32_t level_index;
	uint32_t synthetic_level;
	uint64_t compressed_used_slots;
	uint64_t compressed_allocated_slots;
	uint64_t decoded_used_slots;
	uint64_t decoded_allocated_slots;
} pyra_cache_usage;

// Initialize options before changing individual fields. A null options pointer
// to create uses these defaults. A zero decoded budget disables decoded caching;
// compressed budget, image limit, and decoder worker count must be positive.
PYRA_API void pyra_context_options_init ( pyra_context_options *options );
// Static borrowed strings. An unavailable backend has a null version.
PYRA_API pyra_status pyra_get_backend_info ( pyra_backend backend,
	pyra_backend_info *info, pyra_error *error );
PYRA_API pyra_status pyra_context_create ( const pyra_context_options *options,
	pyra_context **context, pyra_error *error );
// Returns BUSY while WSI handles exist. Destroy/close must not race handle use.
PYRA_API pyra_status pyra_context_destroy ( pyra_context *context, pyra_error *error );
PYRA_API pyra_status pyra_context_get_stats ( pyra_context *context,
	pyra_context_stats *stats, pyra_error *error );
// level_index -1 selects the last actual render level; >= 0 queries a specific
// index in the native WSI's level store. Arena counts span all levels regardless
// of this selection. Before a render, Level mode reports level_index -1 and no
// selected pool. Synthetic levels have no tile cache. OpenSlide is UNSUPPORTED.
PYRA_API pyra_status pyra_get_cache_usage ( pyra_wsi *wsi, int32_t level_index,
	pyra_cache_usage *usage, pyra_error *error );
// Empties the slide's chosen tile caches (PYRA_CACHE_COMPRESSED and/or
// PYRA_CACHE_DECODED) and frees their storage, as if the slide had just been
// opened: the next read of a tile misses. Their lookup counters restart at zero
// (hits, misses, and for the compressed cache bypasses); stored tiles read from
// the source stay counted. Slides sharing the storage (the same source opened
// twice in the context) are emptied too. freed_bytes, optional, receives the
// bytes given back. OpenSlide is UNSUPPORTED.
PYRA_API pyra_status pyra_clear_cache ( pyra_wsi *wsi, uint32_t caches, uint64_t *freed_bytes,
	pyra_error *error );

// Paths and returned strings are UTF-8. Opens of the same canonical source in
// one context share storage. Sources must remain unchanged until all handles close.
// Ordinary TIFF images open as a gallery: a ".glr" manifest laying a directory
// of them out as one slide (see the pyra_make_gallery tool).
// Lay a directory of ordinary images out as a gallery and write its manifest,
// "{directory}/{directory name}.glr" ("{directory}/{slide}.glr" for an IPX
// file-set). Members are the single-page TIFFs directly in the directory, in
// natural name order; their pixel size comes from an ".imd" beside them, then
// from microscope TIFF resolution tags, then from `mpp` (0 = none, which refuses
// a set that states no size). An existing manifest is replaced. On success the
// manifest path is written to `manifest_utf8` (NUL-terminated, truncated to
// `capacity`); the error names what was refused.
PYRA_API pyra_status pyra_make_gallery ( const char *directory_utf8, double mpp,
	char *manifest_utf8, size_t capacity, pyra_error *error );

// Tile compressions a writer can produce.
typedef uint32_t pyra_codec;
enum {
	PYRA_CODEC_JPEG = 1,
	PYRA_CODEC_JPEG2000 = 2,
	PYRA_CODEC_HTJ2K = 3,
	PYRA_CODEC_JPEGXL = 4,
	// PTF only (TIFF compression 22610, as Bio-Formats reads it); not in DICOM.
	PYRA_CODEC_JPEGXR = 5,
	// Untiled TIFF only (tile_edge 0): TIFF compression 5, lossless.
	PYRA_CODEC_LZW = 6
};
#define PYRA_QUALITY_LOSSLESS 101u

// pyra_make_synthetic limits. Tile edges are multiples of PYRA_SYNTHETIC_TILE_STEP.
#define PYRA_SYNTHETIC_TILE_MIN 64u
#define PYRA_SYNTHETIC_TILE_MAX 2048u
#define PYRA_SYNTHETIC_TILE_STEP 16u
#define PYRA_SYNTHETIC_EDGE_MAX 1048576u
// tile_edge 0 writes an untiled TIFF, whose edges are capped lower.
#define PYRA_SYNTHETIC_UNTILED_EDGE_MAX 16384u
#define PYRA_SYNTHETIC_Z_MAX 16u
#define PYRA_SYNTHETIC_MPP_MIN 0.05
#define PYRA_SYNTHETIC_MPP_MAX 10.0
#define PYRA_SYNTHETIC_Z_STEP_MIN_UM 0.1
#define PYRA_SYNTHETIC_Z_STEP_MAX_UM 5.0
#define PYRA_SYNTHETIC_NUCLEUS_MIN_UM 3.0
#define PYRA_SYNTHETIC_NUCLEUS_MAX_UM 40.0
#define PYRA_SYNTHETIC_SPACING_MIN_UM 4.0
#define PYRA_SYNTHETIC_SPACING_MAX_UM 200.0

// 1 when this build can encode `codec`, 0 otherwise (including unknown values).
PYRA_API int pyra_codec_can_encode ( pyra_codec codec );
// 1 when pyra_make_synthetic can write `codec` with this tile edge: 0 (untiled)
// takes LZW and JPEG, a tiled pyramid the other codecs. Build availability included.
PYRA_API int pyra_synthetic_can_encode ( pyra_codec codec, uint32_t tile_edge );

// Called after each written tile; return 0 to cancel the write.
typedef int ( *pyra_progress_fn ) ( void *user, uint64_t done, uint64_t total );

typedef struct pyra_synthetic_options {
	uint32_t struct_size;
	uint32_t width;
	uint32_t height;
	// 0 writes an untiled TIFF (see pyra_make_synthetic).
	uint32_t tile_edge;
	uint32_t z_levels;
	pyra_codec codec;
	// 1..100 lossy; PYRA_QUALITY_LOSSLESS for JPEG 2000, HTJ2K, JPEG XL and JPEG XR.
	// LZW ignores it.
	uint32_t quality;
	uint32_t reserved;
	double mpp;
	// Distance between focal planes, microns.
	double z_step_um;
	// In-focus depth as a fraction of the stack height: 0 = z 0, 1 = last plane.
	double focus_fraction;
	// Tumour nuclei: mean equivalent-circle diameter 2*sqrt(a*b), and spacing as
	// sqrt(area per nucleus) inside tumour nests, microns.
	double nucleus_diameter_um;
	double nucleus_spacing_um;
	// Expected share of DAB-positive tumour nuclei (positive cells cluster).
	double positive_fraction;
	// Expected share of the tissue area inside tumour nests.
	double tumour_fraction;
	// Share of the stromal nuclei that are lymphocytes; the rest are fibroblasts.
	double lymphocyte_fraction;
	uint64_t seed;
	pyra_progress_fn progress;
	void *progress_user;
} pyra_synthetic_options;

// Defaults: 4096 x 4096, 256-pixel tiles, 4 focal planes 1 um apart in focus at
// 5/12 of the stack, 0.5 um per pixel, JPEG quality 90, 8 um tumour nuclei 12 um
// apart, 30% positive, 40% tumour area, 40% lymphocytes in stroma, a fixed seed.
PYRA_API void pyra_synthetic_options_init ( pyra_synthetic_options *options );
// Write a synthetic bright-field IHC slide - hematoxylin and DAB nuclei, stroma,
// white margins - as a PTF z-stack: one RGB8 level ladder per focal plane, the
// tissue in focus at focus_fraction of the stack height (by default a 4-plane
// stack is sharp at z 1, slightly blurred at z 2, blurred at z 0 and 3). The same
// options and seed give the same pixels. An existing file is replaced; a
// cancelled or failed write leaves no file. Out-of-range options and codecs this
// build cannot encode return INVALID_ARGUMENT / UNSUPPORTED naming the field or
// build option.
// With tile_edge 0 the output is instead a plain untiled TIFF (name it .tif): the
// base level of one focal plane (z_levels must be 1), LZW or JPEG strips, edges
// up to PYRA_SYNTHETIC_UNTILED_EDGE_MAX. PyrA opens it through pyra_make_gallery.
PYRA_API pyra_status pyra_make_synthetic ( const pyra_synthetic_options *options,
	const char *path_utf8, pyra_error *error );

// Whether this build can write a synthetic DICOM WSI using this codec.
PYRA_API int pyra_synthetic_dicom_can_encode ( pyra_codec codec );
// The same tissue and focal planes in a DICOM WSI pyramid. The path names a
// .dcm entry file and lower resolutions go in a sibling _<stem>_ directory.
// tile_edge 0 is refused.
// Existing outputs are refused. The progress callback can also be called
// between tiles during DICOM metadata writing and publication.
PYRA_API pyra_status pyra_make_synthetic_dicom ( const pyra_synthetic_options *options,
	const char *path_utf8, pyra_error *error );

// Ground truth of a synthetic slide: the nuclei centred in a base-level
// rectangle. Lymphocytes and fibroblasts have their own small background
// positivity, not counted here.
typedef struct pyra_synthetic_nuclei {
	uint32_t struct_size;
	uint32_t reserved;
	uint64_t tumour;
	uint64_t tumour_positive;
	uint64_t fibroblast;
	uint64_t lymphocyte;
	// Mean equivalent-circle diameter of the tumour nuclei, 0 when there are none.
	double tumour_diameter_um;
} pyra_synthetic_nuclei;

// Count the nuclei of the slide pyra_make_synthetic would write with `options`,
// whose centre lies in base pixels [x, x + width) x [y, y + height), clipped to
// the slide. Nothing is rendered or written; codec and quality are not checked.
PYRA_API pyra_status pyra_synthetic_count_nuclei ( const pyra_synthetic_options *options,
	uint32_t x, uint32_t y, uint32_t width, uint32_t height,
	pyra_synthetic_nuclei *nuclei, pyra_error *error );

PYRA_API pyra_status pyra_open ( pyra_context *context, const char *path_utf8,
	pyra_wsi **wsi, pyra_error *error );
PYRA_API pyra_status pyra_close ( pyra_wsi *wsi, pyra_error *error );
PYRA_API pyra_status pyra_get_plane_count ( pyra_wsi *wsi,
	uint32_t *count, pyra_error *error );
// channel/key/value pointers are borrowed and valid until this WSI handle closes.
PYRA_API pyra_status pyra_get_plane_info ( pyra_wsi *wsi, uint32_t plane_index,
	pyra_plane_info *info, pyra_error *error );
PYRA_API pyra_status pyra_get_property_count ( pyra_wsi *wsi,
	uint32_t *count, pyra_error *error );
PYRA_API pyra_status pyra_get_property ( pyra_wsi *wsi, uint32_t index,
	pyra_property *property, pyra_error *error );

// Dimensions are ceil(rectangle extent / mpp), snapping ratios within 1e-9
// relative error of an integer to that integer. Layout and rendering agree.
// Rendering samples the full half-open rectangle, with zero outside the plane.
PYRA_API pyra_status pyra_get_image_layout ( pyra_wsi *wsi,
	const pyra_image_request *request, pyra_image_layout *layout, pyra_error *error );
// Synchronous; retains no destination memory. Rows are top-down, RGB interleaved,
// integers native-endian, float IEEE-754 binary32. Padding is left untouched.
// Capacity must cover (height - 1) * stride + packed_row_bytes. On INCOMPLETE the
// buffer contains available pixels with zero in missing areas. Other failures
// leave the destination unchanged. All C++ exceptions are translated to status.
// Unavailable decoders return UNSUPPORTED and name the compression/build option.
PYRA_API pyra_status pyra_get_image ( pyra_wsi *wsi,
	const pyra_image_request *request, const pyra_image_buffer *destination,
	pyra_error *error );

// Remote ("sister") slides. A server holding the file describes an open slide
// once with pyra_get_geometry and serves each tile's stored bytes unchanged with
// pyra_read_encoded_tile. A client without the file opens the description with
// pyra_open_remote and renders with pyra_get_image as usual: tiles it does not
// hold yet are rendered as background, the call returns INCOMPLETE, and
// pyra_take_missing_tiles lists them. After pyra_put_encoded_tile has stored
// their bytes, the same request renders exactly what the server would render.
// Level and tile indices are those of the geometry and are opaque to the caller.
// Synthetic coarse levels the server computed are served as losslessly encoded
// virtual tiles. Remote slides are native-only and not shared between handles.
typedef struct pyra_tile_id {
	uint32_t level_index;
	uint32_t tile_index;
} pyra_tile_id;

// Receives bytes the library owns for the duration of the call.
typedef void ( *pyra_bytes_fn )( void *user, const void *data, size_t size );

PYRA_API pyra_status pyra_get_geometry ( pyra_wsi *wsi, pyra_bytes_fn sink,
	void *user, pyra_error *error );
// A sparse (absent) tile delivers zero bytes. A stored tile is read through the
// compressed cache: copied from its slot when cached, else loaded into one.
PYRA_API pyra_status pyra_read_encoded_tile ( pyra_wsi *wsi, uint32_t level_index,
	uint32_t tile_index, pyra_bytes_fn sink, void *user, pyra_error *error );
PYRA_API pyra_status pyra_open_remote ( pyra_context *context, const void *geometry,
	size_t size, pyra_wsi **wsi, pyra_error *error );
PYRA_API pyra_status pyra_put_encoded_tile ( pyra_wsi *wsi, uint32_t level_index,
	uint32_t tile_index, const void *data, size_t size, pyra_error *error );
// Writes up to `capacity` tiles that renders needed and that hold no bytes yet,
// each once, and removes them from the queue; `count` receives how many.
PYRA_API pyra_status pyra_take_missing_tiles ( pyra_wsi *wsi, pyra_tile_id *tiles,
	size_t capacity, size_t *count, pyra_error *error );

// Standalone images. pyra_encode_image encodes a width x height RGB8, RGB16,
// GRAY8 or GRAY16 raster with `codec` (quality 1..100 lossy, or
// PYRA_QUALITY_LOSSLESS where the codec has a lossless mode) and delivers the
// codestream to `sink`. pyra_decode_image decodes any codestream the reader
// decodes into `destination`'s pixel format; with a null destination it only
// reports the size, otherwise the destination must hold exactly that size.
PYRA_API pyra_status pyra_encode_image ( const pyra_image_buffer *source, uint32_t width,
	uint32_t height, pyra_codec codec, uint32_t quality, pyra_bytes_fn sink, void *user,
	pyra_error *error );
PYRA_API pyra_status pyra_decode_image ( const void *data, size_t size,
	const pyra_image_buffer *destination, uint32_t *width, uint32_t *height, pyra_error *error );

// Operations on live handles are thread-safe and serialized within each context.
// Independent contexts may run concurrently. Cache budgets cover retained tile
// pixel/byte storage, not metadata, synthetic rasters, codec/render temporaries,
// allocator overhead, or caller-owned output. They are not process-memory limits.
// OpenSlide exposes one RGB8 plane and a shared decoded cache. Its compressed
// cache option has no effect; decode_threads bounds the workers reading row
// bands of one call concurrently, with the same output for any count. Cache
// usage/eviction counters unavailable from OpenSlide are PYRA_STAT_UNAVAILABLE
// (decoded usage is 0 when its budget is 0). Other counters retain their usual
// meaning.

#ifdef __cplusplus
}
#endif
