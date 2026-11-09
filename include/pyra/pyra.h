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

#define PYRA_ABI_VERSION 2u
#define PYRA_ERROR_MESSAGE_SIZE 512u
#define PYRA_STAT_UNAVAILABLE UINT64_MAX

typedef struct pyra_context pyra_context;
typedef struct pyra_wsi pyra_wsi;
typedef int32_t pyra_status;
typedef uint32_t pyra_pixel_format;
typedef uint32_t pyra_backend;

enum {
	PYRA_BACKEND_NATIVE = 0,
	PYRA_BACKEND_OPENSLIDE = 1
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
} pyra_context_stats;

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

// Paths and returned strings are UTF-8. Opens of the same canonical source in
// one context share storage. Sources must remain unchanged until all handles close.
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

// Operations on live handles are thread-safe and serialized within each context.
// Independent contexts may run concurrently. Cache budgets cover retained tile
// pixel/byte storage, not metadata, synthetic rasters, codec/render temporaries,
// allocator overhead, or caller-owned output. They are not process-memory limits.
// OpenSlide exposes one RGB8 plane and a shared decoded cache. Its compressed
// cache and decode_threads options have no effect. Cache usage/eviction counters
// unavailable from OpenSlide are PYRA_STAT_UNAVAILABLE (decoded usage is 0 when
// its budget is 0). Other counters retain their usual meaning.

#ifdef __cplusplus
}
#endif
