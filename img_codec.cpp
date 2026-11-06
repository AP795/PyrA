#include "img_codec.h"

#include "img_tiff.h"
#include "pyr_rt.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <setjmp.h>
#include <string>
#include <type_traits>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

#if USE_OPENJPEG
extern "C" {
#include <openjpeg.h>
}
#endif

#if USE_JXL
#define JXL_STATIC_DEFINE
#define JXL_CMS_STATIC_DEFINE
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/encode.h>

#ifdef _MSC_VER
#pragma comment(lib, "brotlicommon.lib")
#pragma comment(lib, "brotlidec.lib")
#pragma comment(lib, "brotlienc.lib")
#pragma comment(lib, "hwy.lib")
#pragma comment(lib, "jxl.lib")
#pragma comment(lib, "jxl_cms.lib")
#endif
#endif

#if USE_CZI
//
// libCZI's public headers put std::string / std::map / std::function members in
// dll-exported classes (C4251) and narrow an int to its enum's underlying type
// (C4244). Both are inside the library's own interface and not actionable here,
// and at /W4 they were some 6000 lines of build output per full rebuild - enough
// to bury our own warnings, which is how four real ones went unnoticed.
#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable : 4251 4244 4458 4864 )
#endif
#include <libCZI.h>
#ifdef _MSC_VER
#pragma warning( pop )
 #ifdef _DEBUG
  #pragma comment(lib, "libCZId.lib")
 #else
  #pragma comment(lib, "libCZI.lib")
 #endif
#endif
#endif

namespace priml {

const char *const img_codec_names[] = {
	"none", "JPEG", "JPEG 2000", "JPEG-LS", "JPEG XL", "CZI sub-block",
	"TIFF", "raw samples", ""
};


const char *img_codec_name ( ImgCodec codec )
{
	auto idx = (int)codec;
	if (idx < 0 || idx > (int)ImgCodec::Last)
		return "?";
	return img_codec_names[idx];
}


std::string_view img_codec_id ( ImgCodec codec )
{
	switch (codec) {
	case ImgCodec::Jpeg: return "jpeg";
	case ImgCodec::Jp2k: return "jp2k";
	case ImgCodec::Jxl: return "jxl";
	default: return {};
	}
}


bool parse_img_codec_id ( std::string_view text, ImgCodec *codec )
{
	if (!codec)
		return false;
	for (auto candidate : { ImgCodec::Jpeg, ImgCodec::Jp2k, ImgCodec::Jxl }) {
		if (text == img_codec_id( candidate )) {
			*codec = candidate;
			return true;
		}
	}
	return false;
}


// Magic-byte table - the single source of truth for format identification.
//
// Order matters twice, and both cases are silent-corruption bugs if got wrong:
//   - JPEG-LS carries JPEG's FF D8 FF prefix, so it must be tested first.
//   - The JPEG XL and JP2 signature boxes share both their length/"0000000C"
//     prefix and their 0D 0A 87 0A tail; only the box type at offset 4
//     ('JXL ' vs 'jP  ') separates them, so 4 bytes are not enough.
static constexpr struct {
	ImgCodec codec;
	uint8_t sig[12];
	int len;
} kSigs[] = {
	{ ImgCodec::JpegLs, { 0xFF, 0xD8, 0xFF, 0xF7 }, 4 },
	{ ImgCodec::Jxl,    { 0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ',
	                      0x0D, 0x0A, 0x87, 0x0A }, 12 },
	{ ImgCodec::Jp2k,   { 0x00, 0x00, 0x00, 0x0C, 'j', 'P', ' ', ' ',
	                      0x0D, 0x0A, 0x87, 0x0A }, 12 },
	{ ImgCodec::Jp2k,   { 0xFF, 0x4F, 0xFF, 0x51 }, 4 },
	{ ImgCodec::Jxl,    { 0xFF, 0x0A }, 2 },
	{ ImgCodec::Jpeg,   { 0xFF, 0xD8, 0xFF }, 3 },
	{ ImgCodec::CziSubBlock, { 'C', 'Z', 'S', 'B' }, 4 },
	{ ImgCodec::Tiff,   { 'I', 'I', 0x2A, 0x00 }, 4 },
	{ ImgCodec::Tiff,   { 'M', 'M', 0x00, 0x2A }, 4 },
	{ ImgCodec::Tiff,   { 'I', 'I', 0x2B, 0x00 }, 4 },		// BigTIFF
	{ ImgCodec::Tiff,   { 'M', 'M', 0x00, 0x2B }, 4 },
	{ ImgCodec::RawSamples, { 'R', 'A', 'W', 'S' }, 4 },
};


ImgCodec sniff_img_codec ( const uint8_t *bf, size_t len )
{
	if (!bf || len == 0)
		return ImgCodec::None;
	for (const auto &s : kSigs) {
		if (len < (size_t)s.len)
			continue;
		if (std::memcmp( bf, s.sig, (size_t)s.len ) == 0)
			return s.codec;
	}
	return ImgCodec::None;
}


// Dump the first failing level-1 tile so a decode bug can be reproduced against
// the exact bytes. One file per process; the extension follows the sniffed
// codec so the dump opens in a viewer.
static void MaybeDumpFailingTile ( ImgCodec codec, const TileDiag &diag,
	const uint8_t *bf, size_t len, const char *reason )
{
	if (diag.level_no != 1 || !bf || len == 0)
		return;

	static std::atomic<bool> dumped = false;
	auto expected = false;
	if (!dumped.compare_exchange_strong( expected, true ))
		return;

	auto ec = std::error_code{};
	std::filesystem::create_directories( "test_out", ec );

	auto ext = codec == ImgCodec::Jp2k ? "jp2"
		: codec == ImgCodec::Jxl ? "jxl" : "jpg";
	char name[128];
	snprintf( name, sizeof( name ), "wsi_decode_fail_L%d_T%d.%s",
		diag.level_no, diag.tile_no, ext );
	auto path = std::filesystem::path( "test_out" ) / name;
	auto out = std::ofstream( path, std::ios::binary );
	if (!out) {
		if (pyr::debug_file()) {
			fprintf( pyr::debug_file(),
				"TILE-DUMP: failed to open '%s' for level=%d tile=%d reason=%s\n",
				path.string().c_str(), diag.level_no, diag.tile_no,
				reason ? reason : "unknown" );
			fflush( pyr::debug_file() );
		}
		return;
	}

	out.write( (const char *)bf, (std::streamsize)len );
	out.close();
	if (pyr::debug_file()) {
		fprintf( pyr::debug_file(),
			"TILE-DUMP: wrote '%s' level=%d tile=%d size=%zu reason=%s\n",
			path.string().c_str(), diag.level_no, diag.tile_no, len,
			reason ? reason : "unknown" );
		fflush( pyr::debug_file() );
	}
}


static void LogDecodeFailure ( ImgCodec codec, const TileDiag &diag,
	const uint8_t *bf, size_t bytes, const char *message )
{
	pyr::plog( "wsi tile fail: stage=decode codec=%s lvl=%d tile=%d bytes=%zu reason=%s\n",
		img_codec_name( codec ), diag.level_no, diag.tile_no, bytes,
		(message && *message) ? message : "unknown" );
	MaybeDumpFailingTile( codec, diag, bf, bytes, message );

	static std::atomic<int> decode_fail_logs = 0;
	auto log_idx = decode_fail_logs.fetch_add( 1, std::memory_order_relaxed );
	if ((diag.level_no == 1 || log_idx < 8) && pyr::debug_file()) {
		fprintf( pyr::debug_file(),
			"TILE-DECODE-FAIL: codec=%s lvl=%d tile=%d bytes=%zu msg=%s\n",
			img_codec_name( codec ), diag.level_no, diag.tile_no, bytes,
			(message && *message) ? message : "unknown" );
		fflush( pyr::debug_file() );
	}
}


static double ElapsedMs ( const std::chrono::steady_clock::time_point &t0 )
{
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0 ).count();
}


// --- JPEG XL ---

static bool SetCodecError ( std::string *error, const std::string &message )
{
	if (error)
		*error = message;
	return false;
}


static bool SetJxlError ( std::string *error, const std::string &message )
{
	return SetCodecError( error, message );
}


template <typename pixeltype>
static bool SamplesFitDeclaredDepth ( const Pwhl<pixeltype> &image,
	int bits_per_sample, const char *codec, std::string *error )
{
	if constexpr (std::is_same_v<pixeltype, float>) {
		return true;
	} else {
		using sampletype = channel_type<pixeltype>;
		constexpr auto container_bits = (int)( sizeof( sampletype ) * 8 );
		if (bits_per_sample <= 0 || bits_per_sample >= container_bits)
			return true;
		const auto maximum = (uint32_t{ 1 } << bits_per_sample) - 1u;
		for (auto y = 0; y < image.h; ++y) {
			const auto *row = image.rowp( y );
			for (auto x = 0; x < image.w; ++x) {
				if constexpr (is_rgb_pixel_v<pixeltype>) {
					if (row[x].r <= maximum && row[x].g <= maximum &&
						row[x].b <= maximum)
					{
						continue;
					}
				}
				else if ((uint32_t)row[x] <= maximum) {
					continue;
				}
				return SetCodecError( error, std::string( codec ) +
					" source sample exceeds the declared " +
					std::to_string( bits_per_sample ) + "-bit range" );
			}
		}
		return true;
	}
}


#if USE_JXL


static bool ReadJxlBasicInfo ( const uint8_t *bf, size_t len,
	JxlBasicInfo *info, std::string *error )
{
	if (!bf || len == 0 || !info)
		return SetJxlError( error, "JPEG XL input is empty" );

	auto decoder = std::unique_ptr<JxlDecoder, decltype( &JxlDecoderDestroy )>(
		JxlDecoderCreate( nullptr ), JxlDecoderDestroy );
	if (!decoder)
		return SetJxlError( error, "JxlDecoderCreate failed" );
	if (JxlDecoderSubscribeEvents( decoder.get(), JXL_DEC_BASIC_INFO ) != JXL_DEC_SUCCESS)
		return SetJxlError( error, "JxlDecoderSubscribeEvents failed" );
	if (JxlDecoderSetInput( decoder.get(), bf, len ) != JXL_DEC_SUCCESS)
		return SetJxlError( error, "JxlDecoderSetInput failed" );
	JxlDecoderCloseInput( decoder.get() );

	for (;;) {
		auto status = JxlDecoderProcessInput( decoder.get() );
		if (status == JXL_DEC_BASIC_INFO) {
			if (JxlDecoderGetBasicInfo( decoder.get(), info ) != JXL_DEC_SUCCESS)
				return SetJxlError( error, "JxlDecoderGetBasicInfo failed" );
			return true;
		}
		if (status == JXL_DEC_ERROR)
			return SetJxlError( error, "invalid JPEG XL codestream" );
		if (status == JXL_DEC_NEED_MORE_INPUT || status == JXL_DEC_SUCCESS)
			return SetJxlError( error, "JPEG XL codestream has no basic image information" );
	}
}


struct JxlDecodedPixels {
	JxlBasicInfo info{};
	JxlPixelFormat format{};
	std::vector<uint8_t> bytes;
};


static bool DecodeJxlPixels ( const uint8_t *bf, size_t len,
	JxlDataType data_type, JxlDecodedPixels *out, std::string *error )
{
	if (!bf || len == 0 || !out)
		return SetJxlError( error, "JPEG XL input is empty" );

	auto decoder = std::unique_ptr<JxlDecoder, decltype( &JxlDecoderDestroy )>(
		JxlDecoderCreate( nullptr ), JxlDecoderDestroy );
	if (!decoder)
		return SetJxlError( error, "JxlDecoderCreate failed" );
	if (JxlDecoderSubscribeEvents( decoder.get(),
			JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE ) !=
		JXL_DEC_SUCCESS)
	{
		return SetJxlError( error, "JxlDecoderSubscribeEvents failed" );
	}
	if (JxlDecoderSetInput( decoder.get(), bf, len ) != JXL_DEC_SUCCESS)
		return SetJxlError( error, "JxlDecoderSetInput failed" );
	JxlDecoderCloseInput( decoder.get() );

	auto have_info = false;
	auto have_image = false;
	for (;;) {
		auto status = JxlDecoderProcessInput( decoder.get() );
		switch (status) {
			case JXL_DEC_BASIC_INFO: {
				if (JxlDecoderGetBasicInfo( decoder.get(), &out->info ) != JXL_DEC_SUCCESS)
					return SetJxlError( error, "JxlDecoderGetBasicInfo failed" );
				if (out->info.xsize == 0 || out->info.ysize == 0 ||
					out->info.xsize > (uint32_t)std::numeric_limits<int>::max() ||
					out->info.ysize > (uint32_t)std::numeric_limits<int>::max())
				{
					return SetJxlError( error, "JPEG XL image dimensions are invalid" );
				}
				if (out->info.have_animation)
					return SetJxlError( error, "animated JPEG XL images are not supported" );
				if (out->info.num_color_channels != 1 &&
					out->info.num_color_channels != 3)
				{
					return SetJxlError( error, "JPEG XL image must have one or three color channels" );
				}
				out->format = JxlPixelFormat{
					out->info.num_color_channels, data_type, JXL_NATIVE_ENDIAN, 0 };
				have_info = true;
				break;
			}
			case JXL_DEC_COLOR_ENCODING: {
				if (!have_info)
					return SetJxlError( error, "JPEG XL color information preceded basic information" );
				auto color = JxlColorEncoding{};
				if (data_type == JXL_TYPE_FLOAT)
					JxlColorEncodingSetToLinearSRGB( &color,
						out->info.num_color_channels == 1 ? JXL_TRUE : JXL_FALSE );
				else
					JxlColorEncodingSetToSRGB( &color,
						out->info.num_color_channels == 1 ? JXL_TRUE : JXL_FALSE );
				if (JxlDecoderSetOutputColorProfile(
						decoder.get(), &color, nullptr, 0 ) != JXL_DEC_SUCCESS)
				{
					return SetJxlError( error, "JxlDecoderSetOutputColorProfile failed" );
				}
				break;
			}
			case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
				if (!have_info)
					return SetJxlError( error, "JPEG XL requested pixels before basic information" );
				auto bytes = size_t{ 0 };
				if (JxlDecoderImageOutBufferSize(
						decoder.get(), &out->format, &bytes ) != JXL_DEC_SUCCESS || bytes == 0)
				{
					return SetJxlError( error, "JxlDecoderImageOutBufferSize failed" );
				}
				try {
					out->bytes.resize( bytes );
				} catch (const std::bad_alloc &) {
					return SetJxlError( error, "JPEG XL output buffer allocation failed" );
				}
				if (JxlDecoderSetImageOutBuffer( decoder.get(), &out->format,
						out->bytes.data(), out->bytes.size() ) != JXL_DEC_SUCCESS)
				{
					return SetJxlError( error, "JxlDecoderSetImageOutBuffer failed" );
				}
//
// A codestream that declares a depth between the two integer container sizes -
// 12-bit sensor data being the reason this exists - is by default rescaled to
// fill the output type, multiplying every sample by 65535/4095. Asking for the
// codestream's own depth returns the samples unscaled, which is the only thing
// that makes such a file a lossless round trip.
//
// Deliberately narrow. At 8 or 16 the two settings agree, and outside that range
// this would change what every JPEG XL image already decodes to; 9..15 bits is
// exactly the case nothing produced before this format existed.
				if (data_type == JXL_TYPE_UINT16 &&
					out->info.bits_per_sample > 8 && out->info.bits_per_sample < 16 &&
					out->info.exponent_bits_per_sample == 0)
				{
					auto depth = JxlBitDepth{};
					depth.type = JXL_BIT_DEPTH_FROM_CODESTREAM;
					if (JxlDecoderSetImageOutBitDepth( decoder.get(), &depth ) !=
						JXL_DEC_SUCCESS)
					{
						return SetJxlError( error,
							"JxlDecoderSetImageOutBitDepth failed" );
					}
				}
				break;
			}
			case JXL_DEC_FULL_IMAGE:
				have_image = true;
				break;
			case JXL_DEC_SUCCESS:
				return have_info && have_image && !out->bytes.empty()
					? true : SetJxlError( error, "JPEG XL decode produced no image" );
			case JXL_DEC_NEED_MORE_INPUT:
				return SetJxlError( error, "truncated JPEG XL codestream" );
			case JXL_DEC_ERROR:
				return SetJxlError( error, "JPEG XL decode failed" );
			default:
				break;
		}
	}
}


template <typename pixeltype>
static constexpr JxlDataType JxlDataTypeFor ()
{
	if constexpr (std::is_same_v<channel_type<pixeltype>, uint16_t>)
		return JXL_TYPE_UINT16;
	else if constexpr (std::is_same_v<pixeltype, float>)
		return JXL_TYPE_FLOAT;
	else
		return JXL_TYPE_UINT8;
}


template <typename sampletype>
static sampletype JxlLuminance ( sampletype r, sampletype g, sampletype b )
{
	if constexpr (std::is_same_v<sampletype, float>)
		return 0.299f * r + 0.587f * g + 0.114f * b;
	else {
		auto value = 0.299 * (double)r + 0.587 * (double)g + 0.114 * (double)b;
		return (sampletype)std::clamp<double>( std::lround( value ), 0.0,
			(double)std::numeric_limits<sampletype>::max() );
	}
}


template <typename pixeltype>
static bool CopyJxlPixels ( const JxlDecodedPixels &decoded,
	const TileDst<pixeltype> &dst, std::string *error )
{
	using sampletype = channel_type<pixeltype>;
	static_assert( std::is_same_v<sampletype, uint8_t> ||
		std::is_same_v<sampletype, uint16_t> || std::is_same_v<sampletype, float> );
	static_assert( sizeof( Rgb8 ) == 3 && sizeof( Rgb16 ) == 6 );

	auto w = (int)decoded.info.xsize;
	auto h = (int)decoded.info.ysize;
	auto channels = (int)decoded.info.num_color_channels;
	auto expected = (size_t)w * (size_t)h * (size_t)channels * sizeof( sampletype );
	if (decoded.bytes.size() != expected)
		return SetJxlError( error, "JPEG XL output buffer size is inconsistent" );

	auto stride = w;
	auto *target = dst.reserve( w, h, stride );
	if (!target || stride < w)
		return SetJxlError( error, "JPEG XL destination allocation failed" );
	auto *samples = reinterpret_cast<const sampletype *>( decoded.bytes.data() );
	for (auto y = 0; y < h; ++y) {
		auto *row = target + (size_t)y * (size_t)stride;
		auto *src = samples + (size_t)y * (size_t)w * (size_t)channels;
		for (auto x = 0; x < w; ++x) {
			if constexpr (is_rgb_pixel_v<pixeltype>) {
				if (channels == 1) {
					auto v = src[x];
					row[x] = pixeltype{ v, v, v };
				} else {
					auto i = (size_t)x * 3;
					row[x] = pixeltype{ src[i], src[i + 1], src[i + 2] };
				}
			} else if (channels == 1) {
				row[x] = src[x];
			} else {
				auto i = (size_t)x * 3;
				row[x] = JxlLuminance( src[i], src[i + 1], src[i + 2] );
			}
		}
	}
	return true;
}


template <typename pixeltype>
static bool DecodeJxl ( const uint8_t *bf, size_t len,
	const TileDst<pixeltype> &dst, TileDiag &diag )
{
	auto decoded = JxlDecodedPixels{};
	auto error = std::string{};
	if (!DecodeJxlPixels( bf, len, JxlDataTypeFor<pixeltype>(), &decoded, &error ) ||
		!CopyJxlPixels( decoded, dst, &error ))
	{
		LogDecodeFailure( ImgCodec::Jxl, diag, bf, len, error.c_str() );
		return false;
	}
	return true;
}


template <typename pixeltype>
static bool DecodeJxlOwning ( const uint8_t *bf, size_t len,
	Pwhl<pixeltype> *out, std::string *error )
{
	if (!out)
		return SetJxlError( error, "JPEG XL output image is null" );
	auto dst = TileDst<pixeltype>{};
	dst.reserve = [out] ( int w, int h, int &stride ) -> pixeltype * {
		*out = Pwhl<pixeltype>( Dim2D<int>{ w, h } );
		if (!out->p)
			return nullptr;
		stride = out->l;
		return out->p;
	};
	auto decoded = JxlDecodedPixels{};
	return DecodeJxlPixels( bf, len, JxlDataTypeFor<pixeltype>(), &decoded, error ) &&
		CopyJxlPixels( decoded, dst, error );
}


template <typename pixeltype>
static bool PackJxlInput ( const Pwhl<pixeltype> &image,
	std::vector<pixeltype> *packed, const pixeltype **pixels, size_t *bytes,
	std::string *error )
{
	if (!packed || !pixels || !bytes || !image.p || image.w <= 0 || image.h <= 0 ||
		image.l < image.w)
	{
		return SetJxlError( error, "JPEG XL source image is empty or has an invalid stride" );
	}
	if constexpr (std::is_same_v<pixeltype, float>) {
		for (auto y = 0; y < image.h; ++y)
			for (auto x = 0; x < image.w; ++x)
				if (!std::isfinite( image.rowp( y )[x] ))
					return SetJxlError( error, "JPEG XL cannot encode NaN or infinite samples" );
	}
	if (image.l == image.w) {
		*pixels = image.p;
	} else {
		packed->resize( (size_t)image.w * (size_t)image.h );
		for (auto y = 0; y < image.h; ++y)
			std::copy_n( image.rowp( y ), image.w,
				packed->data() + (size_t)y * (size_t)image.w );
		*pixels = packed->data();
	}
	*bytes = (size_t)image.w * (size_t)image.h * sizeof( pixeltype );
	return true;
}


template <typename pixeltype>
static bool EncodeJxlTyped ( const Pwhl<pixeltype> &image,
	std::vector<uint8_t> *out, const TileEncodeOptions &options,
	std::string *error )
{
	static_assert( is_rgb_pixel_v<pixeltype> ||
		std::is_same_v<pixeltype, uint8_t> ||
		std::is_same_v<pixeltype, uint16_t> ||
		std::is_same_v<pixeltype, float> );
	using sampletype = channel_type<pixeltype>;
	if (!out)
		return SetJxlError( error, "JPEG XL output buffer is null" );

	auto packed = std::vector<pixeltype>{};
	const pixeltype *pixels = nullptr;
	auto input_bytes = size_t{ 0 };
	if (!PackJxlInput( image, &packed, &pixels, &input_bytes, error ))
		return false;

	auto encoder = std::unique_ptr<JxlEncoder, decltype( &JxlEncoderDestroy )>(
		JxlEncoderCreate( nullptr ), JxlEncoderDestroy );
	if (!encoder)
		return SetJxlError( error, "JxlEncoderCreate failed" );

	auto info = JxlBasicInfo{};
	JxlEncoderInitBasicInfo( &info );
	info.xsize = (uint32_t)image.w;
	info.ysize = (uint32_t)image.h;
	info.num_color_channels = is_rgb_pixel_v<pixeltype> ? 3u : 1u;
	auto container_bits = std::is_same_v<sampletype, uint16_t> ? 16u
		: std::is_same_v<sampletype, float> ? 32u : 8u;
	if (!SamplesFitDeclaredDepth(
			image, options.bits_per_sample, "JPEG XL", error ))
	{
		return false;
	}
//
// A narrower declared depth is only meaningful for the integer types, and only
// when it actually fits the container: an out-of-range request is ignored rather
// than clamped, because silently coding at a different depth than asked is worse
// than coding at the safe one.
	info.bits_per_sample = container_bits;
	if constexpr (!std::is_same_v<pixeltype, float>) {
		if (options.bits_per_sample > 0 &&
			(uint32_t)options.bits_per_sample <= container_bits)
		{
			info.bits_per_sample = (uint32_t)options.bits_per_sample;
		}
	}
	info.exponent_bits_per_sample = std::is_same_v<pixeltype, float> ? 8u : 0u;
	info.uses_original_profile = JXL_TRUE;
	if (JxlEncoderSetBasicInfo( encoder.get(), &info ) != JXL_ENC_SUCCESS)
		return SetJxlError( error, "JxlEncoderSetBasicInfo failed" );

	auto color = JxlColorEncoding{};
	if constexpr (std::is_same_v<pixeltype, float>)
		JxlColorEncodingSetToLinearSRGB( &color, JXL_TRUE );
	else
		JxlColorEncodingSetToSRGB( &color,
			is_rgb_pixel_v<pixeltype> ? JXL_FALSE : JXL_TRUE );
	if (JxlEncoderSetColorEncoding( encoder.get(), &color ) != JXL_ENC_SUCCESS)
		return SetJxlError( error, "JxlEncoderSetColorEncoding failed" );

	auto *frame = JxlEncoderFrameSettingsCreate( encoder.get(), nullptr );
	if (!frame)
		return SetJxlError( error, "JxlEncoderFrameSettingsCreate failed" );
	const auto quality = std::clamp(
		options.quality, kImageQualityMin, kImageLosslessQuality );
	if (quality == kImageLosslessQuality) {
		if (JxlEncoderSetFrameLossless( frame, JXL_TRUE ) != JXL_ENC_SUCCESS)
			return SetJxlError( error, "JxlEncoderSetFrameLossless failed" );
	} else {
		if (JxlEncoderSetFrameLossless( frame, JXL_FALSE ) != JXL_ENC_SUCCESS)
			return SetJxlError( error, "JxlEncoderSetFrameLossless failed" );
		const auto distance = quality == kImageLossyQualityMax
			? 0.1f : JxlEncoderDistanceFromQuality( (float)quality );
		if (JxlEncoderSetFrameDistance( frame, distance ) != JXL_ENC_SUCCESS)
			return SetJxlError( error, "JxlEncoderSetFrameDistance failed" );
	}
//
// The counterpart of the decoder's bit-depth override. Without it libjxl reads a
// uint16 buffer as spanning the full 16-bit range and multiplies every sample by
// 4095/65535 on the way into a 12-bit codestream - which is not a smaller
// encoding of the same image, it is the low four bits deleted. Saying the input
// already speaks the codestream's depth is what makes the narrow encoding
// lossless; the round trip is pinned by test_ptf_jxl_bits_per_sample.
	if (info.bits_per_sample != container_bits) {
		auto depth = JxlBitDepth{};
		depth.type = JXL_BIT_DEPTH_FROM_CODESTREAM;
		if (JxlEncoderSetFrameBitDepth( frame, &depth ) != JXL_ENC_SUCCESS)
			return SetJxlError( error, "JxlEncoderSetFrameBitDepth failed" );
	}
	auto format = JxlPixelFormat{
		info.num_color_channels, JxlDataTypeFor<pixeltype>(), JXL_NATIVE_ENDIAN, 0 };
	if (JxlEncoderAddImageFrame( frame, &format, pixels, input_bytes ) != JXL_ENC_SUCCESS)
		return SetJxlError( error, "JxlEncoderAddImageFrame failed" );
	JxlEncoderCloseInput( encoder.get() );

	out->assign( 64 * 1024, 0 );
	auto *next = out->data();
	auto available = out->size();
	for (;;) {
		auto status = JxlEncoderProcessOutput( encoder.get(), &next, &available );
		auto written = (size_t)(next - out->data());
		if (status == JXL_ENC_SUCCESS) {
			out->resize( written );
			return !out->empty() ? true
				: SetJxlError( error, "JPEG XL encoder produced no bytes" );
		}
		if (status != JXL_ENC_NEED_MORE_OUTPUT)
			return SetJxlError( error, "JPEG XL encode failed" );
		if (out->size() > std::numeric_limits<size_t>::max() / 2)
			return SetJxlError( error, "JPEG XL output is too large" );
		out->resize( out->size() * 2 );
		next = out->data() + written;
		available = out->size() - written;
	}
}

#endif // USE_JXL


// --- JPEG (libjpeg-turbo) ---

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324) // structure padded due to alignment (jmp_buf)
#endif
struct decode_error_mgr {
	struct jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
	char message[JMSG_LENGTH_MAX] = {};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif


static void decode_error_exit ( j_common_ptr cinfo )
{
	auto *myerr = (decode_error_mgr *)cinfo->err;
	(*cinfo->err->format_message)( cinfo, myerr->message );
	longjmp( myerr->setjmp_buffer, 1 );
}


static bool ReadJpegInfo ( const uint8_t *bf, size_t len, ImgCodecInfo *out,
	std::string *error )
{
	auto cinfo = jpeg_decompress_struct{};
	auto jerr = decode_error_mgr{};
	cinfo.err = jpeg_std_error( &jerr.pub );
	jerr.pub.error_exit = decode_error_exit;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4611) // setjmp + C++ object destruction
#endif
	if (setjmp( jerr.setjmp_buffer )) {
		jpeg_destroy_decompress( &cinfo );
		return SetCodecError( error,
			std::string( "JPEG header read failed: " ) + jerr.message );
	}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

	jpeg_create_decompress( &cinfo );
	jpeg_mem_src( &cinfo, bf, static_cast<unsigned long>(len) );
	if (jpeg_read_header( &cinfo, TRUE ) != JPEG_HEADER_OK) {
		jpeg_destroy_decompress( &cinfo );
		return SetCodecError( error, "jpeg_read_header failed" );
	}
	out->codec = ImgCodec::Jpeg;
	out->width = static_cast<int>(cinfo.image_width);
	out->height = static_cast<int>(cinfo.image_height);
	out->color_channels = cinfo.num_components == 1 ? 1 : 3;
	out->bits_per_sample = cinfo.data_precision;
	out->is_float = false;
	jpeg_destroy_decompress( &cinfo );
	return out->width > 0 && out->height > 0;
}


// A sample of a JPEG deeper than 8 bits to the target type: native value for the
// 16-bit types, narrowed by the stream's own precision (`shift` = precision - 8)
// for the others, so a 12-bit stream read as 8-bit colour is not near-white.
template <typename pixeltype>
static pixeltype PixelFromDeepJpeg ( uint16_t r, uint16_t g, uint16_t b, int shift )
{
	if constexpr (std::is_same_v<pixeltype, Rgb16>)
		return Rgb16{ r, g, b };
	else if constexpr (std::is_same_v<pixeltype, uint16_t>)
		return RgbToLuminance<uint16_t>( r, g, b );
	else
		return PixelFromRgbBytes<pixeltype>(
			(uint8_t)std::min( 255, r >> shift ), (uint8_t)std::min( 255, g >> shift ),
			(uint8_t)std::min( 255, b >> shift ) );
}


// Scanline loop for a JPEG deeper than 8 bits. libjpeg-turbo serves those through
// separate entry points over wider sample types - jpeg12_read_scanlines for 9..12
// bits (12-bit lossy is DICOM Process 4), jpeg16_read_scanlines for lossless
// 13..16 - so the loop is parameterised on both. Errors longjmp back to the
// caller's setjmp, exactly as the 8-bit loop does.
template <typename pixeltype, typename sampletype, typename ReadRows>
static bool ReadDeepJpegRows ( jpeg_decompress_struct &cinfo,
	const TileDst<pixeltype> &dst, sampletype *row_buf, ReadRows read_rows )
{
	auto w = (int)cinfo.output_width;
	auto ch = cinfo.output_components;
	auto stride = w;
	auto *out = dst.reserve( w, (int)cinfo.output_height, stride );
	if (!out)
		return false;
	const auto shift = std::max( 0, cinfo.data_precision - 8 );
	while (cinfo.output_scanline < cinfo.output_height) {
		auto *rp = row_buf;
		read_rows( &cinfo, &rp, 1 );
		auto y = (int)cinfo.output_scanline - 1;
		auto *row = out + (size_t)y * (size_t)stride;
		for (int x = 0; x < w; ++x) {
			const auto i = (size_t)x * (size_t)ch;
			auto r = (uint16_t)row_buf[i];
			auto g = ch >= 3 ? (uint16_t)row_buf[i + 1] : r;
			auto b = ch >= 3 ? (uint16_t)row_buf[i + 2] : r;
			row[x] = PixelFromDeepJpeg<pixeltype>( r, g, b, shift );
		}
	}
	return true;
}


template <typename pixeltype>
static bool DecodeJpeg ( const uint8_t *bf, size_t len,
	const TileDst<pixeltype> &dst, TileDiag &diag )
{
	auto cinfo = jpeg_decompress_struct{};
	auto jerr = decode_error_mgr{};
	cinfo.err = jpeg_std_error( &jerr.pub );
	jerr.pub.error_exit = decode_error_exit;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4611) // setjmp + C++ object destruction
#endif
	if (setjmp( jerr.setjmp_buffer )) {
		LogDecodeFailure( ImgCodec::Jpeg, diag, bf, len, jerr.message );
		jpeg_destroy_decompress( &cinfo );
		return false;
	}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

	jpeg_create_decompress( &cinfo );
	jpeg_mem_src( &cinfo, bf, (unsigned long)len );
	if (jpeg_read_header( &cinfo, TRUE ) != JPEG_HEADER_OK) {
		LogDecodeFailure( ImgCodec::Jpeg, diag, bf, len, "jpeg_read_header failed" );
		jpeg_destroy_decompress( &cinfo );
		return false;
	}

	if (cinfo.data_precision > 8) {
		cinfo.out_color_space = JCS_RGB;
		jpeg_start_decompress( &cinfo );
		const auto samples = (size_t)cinfo.output_width * cinfo.output_components;
		const auto sample_bytes = cinfo.data_precision <= 12
			? sizeof( J12SAMPLE ) : sizeof( J16SAMPLE );
		auto *scanline = (*cinfo.mem->alloc_large)( (j_common_ptr)&cinfo,
			JPOOL_IMAGE, samples * sample_bytes );
		const auto ok = cinfo.data_precision <= 12
			? ReadDeepJpegRows<pixeltype, J12SAMPLE>( cinfo, dst,
				(J12SAMPLE *)scanline, jpeg12_read_scanlines )
			: ReadDeepJpegRows<pixeltype, J16SAMPLE>( cinfo, dst,
				(J16SAMPLE *)scanline, jpeg16_read_scanlines );
		if (!ok) {
			jpeg_destroy_decompress( &cinfo );
			return false;
		}
	} else if constexpr (std::is_same_v<pixeltype, Rgb>) {
// Fast path: libjpeg writes the packed RGB triples straight into the caller's
// buffer, so there is no per-pixel conversion and no intermediate surface.
		cinfo.out_color_space = JCS_EXT_RGB;
		jpeg_start_decompress( &cinfo );

		auto w = (int)cinfo.output_width;
		auto h = (int)cinfo.output_height;
		auto stride = w;
		auto *out = dst.reserve( w, h, stride );
		if (!out) {
			jpeg_destroy_decompress( &cinfo );
			return false;
		}
		while (cinfo.output_scanline < cinfo.output_height) {
			auto y = (int)cinfo.output_scanline;
			auto *rp = (uint8_t *)(out + (size_t)y * (size_t)stride);
			jpeg_read_scanlines( &cinfo, &rp, 1 );
		}
	} else {
		cinfo.out_color_space = JCS_RGB;
		jpeg_start_decompress( &cinfo );

		auto w = (int)cinfo.output_width;
		auto h = (int)cinfo.output_height;
		auto ch = cinfo.output_components;
		auto stride = w;
		auto *out = dst.reserve( w, h, stride );
		if (!out) {
			jpeg_destroy_decompress( &cinfo );
			return false;
		}
		auto *row_buf = (uint8_t *)(*cinfo.mem->alloc_large)(
			(j_common_ptr)&cinfo, JPOOL_IMAGE, (size_t)w * (size_t)ch );
		while (cinfo.output_scanline < cinfo.output_height) {
			auto *rp = row_buf;
			jpeg_read_scanlines( &cinfo, &rp, 1 );
			auto y = (int)cinfo.output_scanline - 1;
			auto *row = out + (size_t)y * (size_t)stride;
			for (int x = 0; x < w; ++x) {
				auto r = row_buf[(size_t)x * (size_t)ch];
				auto g = ch >= 3 ? row_buf[(size_t)x * (size_t)ch + 1] : r;
				auto b = ch >= 3 ? row_buf[(size_t)x * (size_t)ch + 2] : r;
				row[x] = PixelFromRgbBytes<pixeltype>( r, g, b );
			}
		}
	}

	jpeg_finish_decompress( &cinfo );
	jpeg_destroy_decompress( &cinfo );
	return true;
}


template <typename pixeltype>
static bool EncodeJpeg ( const Pwhl<pixeltype> &image,
	std::vector<uint8_t> *out, const TileEncodeOptions &options,
	std::string *error )
{
	if constexpr (!std::is_same_v<pixeltype, Rgb> &&
		!std::is_same_v<pixeltype, uint8_t>)
	{
		return SetCodecError( error,
			"JPEG encoding supports only RGB8 and Gray8 tiles" );
	} else {
		if (!out || !image.p || image.w <= 0 || image.h <= 0 || image.l < image.w)
			return SetCodecError( error, "JPEG source image is empty or invalid" );
		if (options.quality == kImageLosslessQuality)
			return SetCodecError( error, "JPEG has no lossless encoding mode" );

		static_assert( !std::is_same_v<pixeltype, Rgb> || sizeof( Rgb ) == 3 );
		auto cinfo = jpeg_compress_struct{};
		auto jerr = decode_error_mgr{};
		cinfo.err = jpeg_std_error( &jerr.pub );
		jerr.pub.error_exit = decode_error_exit;
		auto *memory = static_cast<unsigned char *>( nullptr );
		unsigned long memory_size = 0;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4611) // setjmp + C++ object destruction
#endif
		if (setjmp( jerr.setjmp_buffer )) {
			jpeg_destroy_compress( &cinfo );
			std::free( memory );
			out->clear();
			return SetCodecError( error,
				std::string( "JPEG encode failed: " ) + jerr.message );
		}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

		jpeg_create_compress( &cinfo );
		jpeg_mem_dest( &cinfo, &memory, &memory_size );
		cinfo.image_width = (JDIMENSION)image.w;
		cinfo.image_height = (JDIMENSION)image.h;
		cinfo.input_components = std::is_same_v<pixeltype, Rgb> ? 3 : 1;
		cinfo.in_color_space = std::is_same_v<pixeltype, Rgb>
			? JCS_EXT_RGB : JCS_GRAYSCALE;
		jpeg_set_defaults( &cinfo );
		jpeg_set_quality( &cinfo, std::clamp( options.quality,
			kImageQualityMin, kImageLossyQualityMax ), TRUE );
		cinfo.optimize_coding = TRUE;
		jpeg_start_compress( &cinfo, TRUE );
		while (cinfo.next_scanline < cinfo.image_height) {
			auto y = (int)cinfo.next_scanline;
			auto *row = reinterpret_cast<JSAMPROW>(
				const_cast<pixeltype *>( image.rowp( y ) ) );
			jpeg_write_scanlines( &cinfo, &row, 1 );
		}
		jpeg_finish_compress( &cinfo );
		out->assign( memory, memory + memory_size );
		jpeg_destroy_compress( &cinfo );
		std::free( memory );
		return !out->empty()
			? true : SetCodecError( error, "JPEG encoder produced no bytes" );
	}
}


// --- JPEG 2000 (OpenJPEG) ---

#if USE_OPENJPEG

// Read cursor over the in-memory codestream, driving the OpenJPEG stream
// callbacks. OpenJPEG has no built-in memory stream, so it is supplied here
// rather than staging the tile through a temporary file.
struct OpjMemSrc {
	const uint8_t *data = nullptr;
	OPJ_SIZE_T size = 0;
	OPJ_SIZE_T pos = 0;
};


struct OpjMemSink {
	std::vector<uint8_t> data;
	OPJ_SIZE_T pos = 0;
};


struct OpjEncodeMessages {
	std::string error;
};


static OPJ_SIZE_T OpjRead ( void *buffer, OPJ_SIZE_T count, void *user )
{
	auto *src = (OpjMemSrc *)user;
	if (src->pos >= src->size)
		return (OPJ_SIZE_T)-1;					// EOF, per the OpenJPEG contract
	auto n = std::min<OPJ_SIZE_T>( count, src->size - src->pos );
	std::memcpy( buffer, src->data + src->pos, n );
	src->pos += n;
	return n;
}


static OPJ_OFF_T OpjSkip ( OPJ_OFF_T count, void *user )
{
	auto *src = (OpjMemSrc *)user;
	if (count < 0)
		return -1;
	auto n = std::min<OPJ_SIZE_T>( (OPJ_SIZE_T)count, src->size - src->pos );
	src->pos += n;
	return (OPJ_OFF_T)n;
}


static OPJ_BOOL OpjSeek ( OPJ_OFF_T pos, void *user )
{
	auto *src = (OpjMemSrc *)user;
	if (pos < 0 || (OPJ_SIZE_T)pos > src->size)
		return OPJ_FALSE;
	src->pos = (OPJ_SIZE_T)pos;
	return OPJ_TRUE;
}


static void OpjWarn ( const char *msg, void *user );
static void OpjEncodeError ( const char *msg, void *user );


static bool ReadJp2kInfo ( const uint8_t *bf, size_t len, ImgCodecInfo *out,
	std::string *error )
{
	auto is_raw = len >= 2 && bf[0] == 0xFF && bf[1] == 0x4F;
	auto *codec = opj_create_decompress( is_raw ? OPJ_CODEC_J2K : OPJ_CODEC_JP2 );
	if (!codec)
		return SetCodecError( error, "opj_create_decompress failed" );
	auto messages = OpjEncodeMessages{};
	opj_set_error_handler( codec, OpjEncodeError, &messages );
	opj_set_warning_handler( codec, OpjWarn, nullptr );
	auto params = opj_dparameters_t{};
	opj_set_default_decoder_parameters( &params );
	if (!opj_setup_decoder( codec, &params )) {
		opj_destroy_codec( codec );
		return SetCodecError( error, "opj_setup_decoder failed" );
	}
	opj_codec_set_threads( codec, 1 );

	auto src = OpjMemSrc{ bf, static_cast<OPJ_SIZE_T>(len), 0 };
	auto *stream = opj_stream_default_create( OPJ_TRUE );
	if (!stream) {
		opj_destroy_codec( codec );
		return SetCodecError( error, "opj_stream_default_create failed" );
	}
	opj_stream_set_user_data( stream, &src, nullptr );
	opj_stream_set_user_data_length( stream, static_cast<OPJ_UINT64>(len) );
	opj_stream_set_read_function( stream, OpjRead );
	opj_stream_set_skip_function( stream, OpjSkip );
	opj_stream_set_seek_function( stream, OpjSeek );

	auto *image = static_cast<opj_image_t *>( nullptr );
	const auto ok = opj_read_header( stream, codec, &image ) == OPJ_TRUE;
	opj_stream_destroy( stream );
	opj_destroy_codec( codec );
	if (!ok || !image) {
		if (image)
			opj_image_destroy( image );
		return SetCodecError( error, messages.error.empty()
			? "JPEG 2000 header read failed" : messages.error );
	}

	out->codec = ImgCodec::Jp2k;
	out->width = static_cast<int>(image->x1 - image->x0);
	out->height = static_cast<int>(image->y1 - image->y0);
	out->color_channels = image->numcomps == 1 ? 1 : 3;
	out->bits_per_sample = 0;
	for (auto i = OPJ_UINT32{ 0 }; i < image->numcomps; ++i)
		out->bits_per_sample = std::max( out->bits_per_sample,
			static_cast<int>(image->comps[i].prec) );
	out->is_float = false;
	opj_image_destroy( image );
	return out->width > 0 && out->height > 0 && out->bits_per_sample > 0;
}


static OPJ_SIZE_T OpjWrite ( void *buffer, OPJ_SIZE_T count, void *user )
{
	auto *dst = (OpjMemSink *)user;
	try {
		if (dst->pos + count > dst->data.size())
			dst->data.resize( dst->pos + count );
	} catch (const std::bad_alloc &) {
		return (OPJ_SIZE_T)-1;
	}
	std::memcpy( dst->data.data() + dst->pos, buffer, count );
	dst->pos += count;
	return count;
}


static OPJ_OFF_T OpjWriteSkip ( OPJ_OFF_T count, void *user )
{
	auto *dst = (OpjMemSink *)user;
	if (count < 0)
		return -1;
	try {
		dst->pos += (OPJ_SIZE_T)count;
		if (dst->pos > dst->data.size())
			dst->data.resize( dst->pos );
	} catch (const std::bad_alloc &) {
		return -1;
	}
	return count;
}


static OPJ_BOOL OpjWriteSeek ( OPJ_OFF_T pos, void *user )
{
	auto *dst = (OpjMemSink *)user;
	if (pos < 0)
		return OPJ_FALSE;
	try {
		dst->pos = (OPJ_SIZE_T)pos;
		if (dst->pos > dst->data.size())
			dst->data.resize( dst->pos );
	} catch (const std::bad_alloc &) {
		return OPJ_FALSE;
	}
	return OPJ_TRUE;
}


static void OpjError ( const char *msg, void *user )
{
	auto *note = (const char *)user;
	pyr::plog( "wsi jp2k: %s%s", note ? note : "", msg ? msg : "(null)\n" );
}


static void OpjWarn ( const char *msg, void * )
{
	if (pyr::debug_file())
		fprintf( pyr::debug_file(), "JP2K-WARN: %s", msg ? msg : "(null)\n" );
}


static void OpjEncodeError ( const char *msg, void *user )
{
	auto *messages = (OpjEncodeMessages *)user;
	if (messages && msg)
		messages->error += msg;
}


// One component sample scaled to 8 bits, with the signed offset undone.
static inline uint8_t OpjSample ( const opj_image_comp_t &c, OPJ_INT32 v )
{
	if (c.sgnd)
		v += 1 << (c.prec - 1);
	if (c.prec > 8)
		v >>= (c.prec - 8);
	else if (c.prec < 8)
		v <<= (8 - c.prec);
	return (uint8_t)std::clamp<OPJ_INT32>( v, 0, 255 );
}


// Sensor samples wider than eight bits keep their numeric code values. An
// 8-bit source requested as uint16 retains the established full-range scaling.
static inline uint16_t OpjSampleWord ( const opj_image_comp_t &c, OPJ_INT32 v )
{
	if (c.sgnd)
		v += 1 << ( c.prec - 1 );
	if (c.prec <= 8) {
		const auto maximum = ( 1 << c.prec ) - 1;
		v = std::clamp<OPJ_INT32>( v, 0, maximum );
		return (uint16_t)( ( (uint32_t)v * 65535u ) / (uint32_t)maximum );
	}
	if (c.prec > 16)
		v >>= ( c.prec - 16 );
	return (uint16_t)std::clamp<OPJ_INT32>( v, 0, 65535 );
}


static float OpjRateForQuality ( int quality )
{
	const auto q = std::clamp( quality, kImageQualityMin,
		kImageLossyQualityMax );
	return (float)std::pow( 20.0, ( kImageLossyQualityMax - q ) / 50.0 );
}


template <typename pixeltype>
static bool EncodeJp2k ( const Pwhl<pixeltype> &source,
	std::vector<uint8_t> *out, const TileEncodeOptions &options,
	std::string *error )
{
	if constexpr (std::is_same_v<pixeltype, float>) {
		return SetCodecError( error,
			"JPEG 2000 encoding does not support Float32 tiles" );
	} else {
		if (!out || !source.p || source.w <= 0 || source.h <= 0 ||
			source.l < source.w)
		{
			return SetCodecError( error,
				"JPEG 2000 source image is empty or invalid" );
		}

		constexpr auto components = is_rgb_pixel_v<pixeltype> ? 3 : 1;
		constexpr auto container_bits =
			std::is_same_v<channel_type<pixeltype>, uint16_t> ? 16 : 8;
//
// A 16-bit container may declare a narrower precision. Unlike JPEG XL there is no
// rescale to undo: OpenJPEG takes the integers as they are, so `prec` is simply
// the truth about their range, and a sample above it would be corrupted - hence
// out-of-range requests are ignored rather than honoured.
		auto precision = container_bits;
		if (options.bits_per_sample > 8 && options.bits_per_sample < container_bits)
			precision = options.bits_per_sample;
		if (!SamplesFitDeclaredDepth(
				source, precision, "JPEG 2000", error ))
		{
			return false;
		}
		auto component_params =
			std::vector<opj_image_cmptparm_t>( (size_t)components );
		for (auto &component : component_params) {
			component = {};
			component.dx = 1;
			component.dy = 1;
			component.w = (OPJ_UINT32)source.w;
			component.h = (OPJ_UINT32)source.h;
			component.prec = (OPJ_UINT32)precision;
			component.sgnd = 0;
		}
		auto *image = opj_image_create( components, component_params.data(),
			components == 3 ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY );
		if (!image)
			return SetCodecError( error, "opj_image_create failed" );
		image->x0 = 0;
		image->y0 = 0;
		image->x1 = (OPJ_UINT32)source.w;
		image->y1 = (OPJ_UINT32)source.h;

		for (auto y = 0; y < source.h; ++y) {
			const auto *row = source.rowp( y );
			for (auto x = 0; x < source.w; ++x) {
				const auto i = (size_t)y * source.w + x;
				if constexpr (is_rgb_pixel_v<pixeltype>) {
					image->comps[0].data[i] = row[x].r;
					image->comps[1].data[i] = row[x].g;
					image->comps[2].data[i] = row[x].b;
				} else {
					image->comps[0].data[i] = row[x];
				}
			}
		}

		auto params = opj_cparameters_t{};
		opj_set_default_encoder_parameters( &params );
		params.tcp_numlayers = 1;
		params.cp_disto_alloc = 1;
		const auto lossless = options.quality == kImageLosslessQuality;
		params.tcp_rates[0] = lossless ? 0.0f : OpjRateForQuality( options.quality );
		params.irreversible = lossless ? 0 : 1;
		params.tcp_mct = components == 3 ? 1 : 0;
		auto shortest = std::min( source.w, source.h );
		params.numresolution = 1;
		while (params.numresolution < 6 &&
			( shortest >> params.numresolution ) >= 1)
		{
			++params.numresolution;
		}

		auto *codec = opj_create_compress( OPJ_CODEC_J2K );
		if (!codec) {
			opj_image_destroy( image );
			return SetCodecError( error, "opj_create_compress failed" );
		}
		auto messages = OpjEncodeMessages{};
		opj_set_error_handler( codec, OpjEncodeError, &messages );
		opj_set_warning_handler( codec, OpjWarn, nullptr );
		opj_codec_set_threads( codec, 1 );
		if (!opj_setup_encoder( codec, &params, image )) {
			opj_destroy_codec( codec );
			opj_image_destroy( image );
			return SetCodecError( error, messages.error.empty()
				? "opj_setup_encoder failed" : messages.error );
		}

		auto sink = OpjMemSink{};
		sink.data.reserve( (size_t)source.w * source.h * components + 4096 );
		auto *stream = opj_stream_default_create( OPJ_FALSE );
		if (!stream) {
			opj_destroy_codec( codec );
			opj_image_destroy( image );
			return SetCodecError( error, "opj_stream_default_create failed" );
		}
		opj_stream_set_user_data( stream, &sink, nullptr );
		opj_stream_set_write_function( stream, OpjWrite );
		opj_stream_set_skip_function( stream, OpjWriteSkip );
		opj_stream_set_seek_function( stream, OpjWriteSeek );

		const auto ok = opj_start_compress( codec, image, stream ) &&
			opj_encode( codec, stream ) && opj_end_compress( codec, stream );
		opj_stream_destroy( stream );
		opj_destroy_codec( codec );
		opj_image_destroy( image );
		if (!ok)
			return SetCodecError( error, messages.error.empty()
				? "JPEG 2000 encode failed" : messages.error );
		sink.data.resize( sink.pos );
		*out = std::move( sink.data );
		return !out->empty()
			? true : SetCodecError( error, "JPEG 2000 encoder produced no bytes" );
	}
}


static inline void YCbCrToRgb ( int y, int cb, int cr,
	uint8_t &r, uint8_t &g, uint8_t &b )
{
	auto fy = (float)y;
	auto fcb = (float)cb - 128.0f;
	auto fcr = (float)cr - 128.0f;
	r = (uint8_t)std::clamp( fy + 1.402f * fcr, 0.0f, 255.0f );
	g = (uint8_t)std::clamp( fy - 0.344136f * fcb - 0.714136f * fcr, 0.0f, 255.0f );
	b = (uint8_t)std::clamp( fy + 1.772f * fcb, 0.0f, 255.0f );
}


// The full-scale value OpjSampleWord maps a component onto: its own code range
// above eight bits, the 16-bit range for the 8-bit sources it widens.
static inline int OpjWordFullScale ( const opj_image_comp_t &c )
{
	return c.prec <= 8 || c.prec >= 16 ? 65535 : ( 1 << c.prec ) - 1;
}


// YCbCrToRgb for 16-bit samples whose full scale is `maximum`; the chroma
// offset is half that range rather than 128.
static inline void YCbCrToRgbWord ( int y, int cb, int cr, int maximum,
	uint16_t &r, uint16_t &g, uint16_t &b )
{
	const auto half = (float)( ( maximum + 1 ) / 2 );
	const auto top = (float)maximum;
	auto fy = (float)y;
	auto fcb = (float)cb - half;
	auto fcr = (float)cr - half;
	r = (uint16_t)std::clamp( fy + 1.402f * fcr, 0.0f, top );
	g = (uint16_t)std::clamp( fy - 0.344136f * fcb - 0.714136f * fcr, 0.0f, top );
	b = (uint16_t)std::clamp( fy + 1.772f * fcb, 0.0f, top );
}


template <typename pixeltype>
static bool DecodeJp2k ( const uint8_t *bf, size_t len, ColorHint hint,
	const TileDst<pixeltype> &dst, TileDiag &diag )
{
// Raw codestreams start with the SOC marker; anything else reaching here is the
// JP2 box container.
	auto is_raw = len >= 2 && bf[0] == 0xFF && bf[1] == 0x4F;
	auto *codec = opj_create_decompress( is_raw ? OPJ_CODEC_J2K : OPJ_CODEC_JP2 );
	if (!codec) {
		LogDecodeFailure( ImgCodec::Jp2k, diag, bf, len, "opj_create_decompress failed" );
		return false;
	}
	opj_set_error_handler( codec, OpjError, (void *)"" );
	opj_set_warning_handler( codec, OpjWarn, nullptr );

	opj_dparameters_t params;
	opj_set_default_decoder_parameters( &params );
	if (!opj_setup_decoder( codec, &params )) {
		LogDecodeFailure( ImgCodec::Jp2k, diag, bf, len, "opj_setup_decoder failed" );
		opj_destroy_codec( codec );
		return false;
	}
// Tiles are already decoded in parallel by the caller's pool; letting OpenJPEG
// spawn its own threads per tile would oversubscribe every core.
	opj_codec_set_threads( codec, 1 );

	auto src = OpjMemSrc{ bf, (OPJ_SIZE_T)len, 0 };
	auto *stream = opj_stream_default_create( OPJ_TRUE );
	if (!stream) {
		LogDecodeFailure( ImgCodec::Jp2k, diag, bf, len, "opj_stream_default_create failed" );
		opj_destroy_codec( codec );
		return false;
	}
	opj_stream_set_user_data( stream, &src, nullptr );
	opj_stream_set_user_data_length( stream, (OPJ_UINT64)len );
	opj_stream_set_read_function( stream, OpjRead );
	opj_stream_set_skip_function( stream, OpjSkip );
	opj_stream_set_seek_function( stream, OpjSeek );

	opj_image_t *image = nullptr;
	auto ok = opj_read_header( stream, codec, &image ) &&
		opj_decode( codec, stream, image ) &&
		opj_end_decompress( codec, stream );

	opj_stream_destroy( stream );
	opj_destroy_codec( codec );

	if (!ok || !image) {
		LogDecodeFailure( ImgCodec::Jp2k, diag, bf, len, "codestream decode failed" );
		if (image)
			opj_image_destroy( image );
		return false;
	}

	auto nc = (int)image->numcomps;
	if (nc < 1 || !image->comps || !image->comps[0].data) {
		LogDecodeFailure( ImgCodec::Jp2k, diag, bf, len, "no image components" );
		opj_image_destroy( image );
		return false;
	}
	if (nc > 3)
		nc = 3;									// ignore alpha / extra channels

// Full-resolution size comes from the reference grid, not from component 0: a
// subsampled component is smaller, and on 4:2:0 even component 0 can be if the
// codestream is unusual.
	auto w = (int)(image->x1 - image->x0);
	auto h = (int)(image->y1 - image->y0);
	if (w <= 0 || h <= 0) {
		LogDecodeFailure( ImgCodec::Jp2k, diag, bf, len, "empty image grid" );
		opj_image_destroy( image );
		return false;
	}
	for (int i = 0; i < nc; ++i) {
		const auto &c = image->comps[i];
		if (!c.data || c.w == 0 || c.h == 0 || c.dx == 0 || c.dy == 0) {
			LogDecodeFailure( ImgCodec::Jp2k, diag, bf, len, "empty component" );
			opj_image_destroy( image );
			return false;
		}
	}

	auto stride = w;
	auto *out = dst.reserve( w, h, stride );
	if (!out) {
		opj_image_destroy( image );
		return false;
	}

// Whether the samples still need YCbCr -> RGB.
//
// Two independent signals, and they must not be confused:
//   - COD MCT=1 (YBR_ICT / YBR_RCT): OpenJPEG inverts it during opj_decode, so
//     the samples are already RGB. Nothing to do, and no post-decode flag says
//     so - the absence of SYCC is what indicates it.
//   - OPJ_CLRSPC_SYCC: set from a JP2 colr box that declares sYCC. opj_decode
//     never performs the sycc->rgb step itself, so this means "still YCbCr".
//     Measured: a raw Aperio 33003 codestream does NOT get this - OpenJPEG
//     leaves the colour space unspecified there, which is precisely why the
//     container hint exists and cannot be replaced by sniffing the stream.
// The two signals are OR-ed because either alone is sufficient evidence. An
// explicit Rgb hint overrides both, for a container known to mislabel.
	auto convert_ycbcr = nc == 3 && hint != ColorHint::Rgb &&
		(hint == ColorHint::YCbCr || image->color_space == OPJ_CLRSPC_SYCC);

// Chroma subsampling: Aperio 33003 stores 4:2:2 (dx=2 on Cb/Cr), so a component
// is read at (x/dx, y/dy) and replicated. Nearest-neighbour, which is what the
// subsampling encoded in the first place; interpolating would invent detail.
	auto sample_at = [&] ( const opj_image_comp_t &c, int x, int y ) {
		auto cx = std::min( (OPJ_UINT32)(x / (int)c.dx), c.w - 1 );
		auto cy = std::min( (OPJ_UINT32)(y / (int)c.dy), c.h - 1 );
		return OpjSample( c, c.data[(size_t)cy * c.w + cx] );
	};
	auto sample_word_at = [&] ( const opj_image_comp_t &c, int x, int y ) {
		auto cx = std::min( (OPJ_UINT32)(x / (int)c.dx), c.w - 1 );
		auto cy = std::min( (OPJ_UINT32)(y / (int)c.dy), c.h - 1 );
		return OpjSampleWord( c, c.data[(size_t)cy * c.w + cx] );
	};

	for (int y = 0; y < h; ++y) {
		auto *row = out + (size_t)y * (size_t)stride;
		if (nc == 1) {
			const auto &c = image->comps[0];
			for (int x = 0; x < w; ++x) {
				auto cx = std::min( (OPJ_UINT32)(x / (int)c.dx), c.w - 1 );
				auto cy = std::min( (OPJ_UINT32)(y / (int)c.dy), c.h - 1 );
				auto sample = c.data[(size_t)cy * c.w + cx];
				if constexpr (std::is_same_v<pixeltype, uint16_t>)
					row[x] = OpjSampleWord( c, sample );
				else if constexpr (std::is_same_v<pixeltype, Rgb16>)
					row[x] = PixelFromGrayWord<pixeltype>( OpjSampleWord( c, sample ) );
				else
					row[x] = PixelFromGrayByte<pixeltype>( OpjSample( c, sample ) );
			}
		} else {
			const auto &c0 = image->comps[0];
			const auto &c1 = image->comps[1];
			const auto &c2 = image->comps[2];
			for (int x = 0; x < w; ++x) {
				if constexpr (std::is_same_v<pixeltype, Rgb16>) {
//	Kept at the codestream's own depth: 12-bit colour stays 0..4095.
					auto a = sample_word_at( c0, x, y );
					auto b = sample_word_at( c1, x, y );
					auto c = sample_word_at( c2, x, y );
					if (convert_ycbcr)
						YCbCrToRgbWord( a, b, c, OpjWordFullScale( c0 ), a, b, c );
					row[x] = Rgb16{ a, b, c };
				} else {
					auto a = sample_at( c0, x, y );
					auto b = sample_at( c1, x, y );
					auto c = sample_at( c2, x, y );
					if (convert_ycbcr)
						YCbCrToRgb( a, b, c, a, b, c );
					row[x] = PixelFromRgbBytes<pixeltype>( a, b, c );
				}
			}
		}
	}

	opj_image_destroy( image );
	return true;
}

#endif // USE_OPENJPEG


#if USE_CZI

// --- CZI sub-block ---
//
// The payload is whatever libCZI stored: JPEG XR, one of the two zstd flavours,
// a JPEG, or nothing at all. libCZI's own decoders are used rather than adding a
// JPEG XR implementation here; ISite::GetDecoder is documented as safe to call
// concurrently, which is what lets these tiles ride the shared decode pool
// instead of being decoded serially while loading.

struct CziTileHeader {
	libCZI::CompressionMode compression = libCZI::CompressionMode::Invalid;
	libCZI::PixelType pixel_type = libCZI::PixelType::Invalid;
	int w = 0;
	int h = 0;
	float black = 0.0f;
	float white = 1.0f;
};


static uint32_t ReadU32Le ( const uint8_t *p )
{
	return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) |
		( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
}


static bool ParseCziTileHeader ( const uint8_t *bf, size_t len,
	CziTileHeader *out )
{
	if (len < (size_t)kCziTileHeaderBytes || bf[4] != kCziTileHeaderVersion)
		return false;

	out->compression = (libCZI::CompressionMode)bf[5];
	out->pixel_type = (libCZI::PixelType)bf[6];
	out->w = (int)ReadU32Le( bf + 8 );
	out->h = (int)ReadU32Le( bf + 12 );
	auto black = ReadU32Le( bf + 16 );
	auto white = ReadU32Le( bf + 20 );
	std::memcpy( &out->black, &black, sizeof( float ) );
	std::memcpy( &out->white, &white, sizeof( float ) );
	return out->w > 0 && out->h > 0;
}


// Map a raw 16-bit sample through the display black/white points. Full range
// (0..1) is the common case and is left exact rather than run through the
// multiply.
static uint16_t ApplyGrayWindow ( uint16_t value, float black, float white )
{
	if (!( white > black ) || ( black <= 0.0f && white >= 1.0f ))
		return value;

	auto lo = black * 65535.0f;
	auto hi = white * 65535.0f;
	auto scaled = ( (float)value - lo ) * 65535.0f / ( hi - lo );
	return (uint16_t)std::clamp( scaled, 0.0f, 65535.0f );
}


// Copy CZI pixels into the destination, converting colour order and grey depth.
//
// Takes a bare pointer rather than an IBitmapData so the uncompressed case can
// be served straight from the sub-block payload: libCZI has no public way to
// wrap an existing buffer in a bitmap, and decoding is not needed to read it.
template <typename pixeltype>
static bool CopyCziPixels ( const uint8_t *base, size_t src_stride,
	libCZI::PixelType type, int w, int h, const CziTileHeader &header,
	const TileDst<pixeltype> &dst )
{
	if (!base || w <= 0 || h <= 0)
		return false;

	auto stride = 0;
	auto *out = dst.reserve( w, h, stride );
	if (!out)
		return false;

	for (auto y = 0; y < h; ++y) {
		const auto *src = base + (size_t)y * src_stride;
		auto *row = out + (size_t)y * (size_t)stride;
		switch (type) {
			case libCZI::PixelType::Bgr24:
//
// CZI stores blue first; every other loader in the tree hands us RGB.
				for (auto x = 0; x < w; ++x, src += 3)
					row[x] = PixelFromRgbBytes<pixeltype>( src[2], src[1], src[0] );
				break;
			case libCZI::PixelType::Bgr48:
				for (auto x = 0; x < w; ++x, src += 6) {
					auto r = (uint16_t)( src[4] | ( src[5] << 8 ) );
					auto g = (uint16_t)( src[2] | ( src[3] << 8 ) );
					auto b = (uint16_t)( src[0] | ( src[1] << 8 ) );
					if constexpr (std::is_same_v<pixeltype, Rgb16>)
						row[x] = PixelFromRgbWords<pixeltype>( r, g, b );
					else
						row[x] = PixelFromRgbBytes<pixeltype>(
							(uint8_t)( r >> 8 ), (uint8_t)( g >> 8 ),
							(uint8_t)( b >> 8 ) );
				}
				break;
			case libCZI::PixelType::Gray8:
				for (auto x = 0; x < w; ++x)
					row[x] = PixelFromGrayWord<pixeltype>( ApplyGrayWindow(
						(uint16_t)( src[x] << 8 | src[x] ),
						header.black, header.white ) );
				break;
			case libCZI::PixelType::Gray16:
				for (auto x = 0; x < w; ++x) {
					auto v = (uint16_t)( src[x * 2] | ( src[x * 2 + 1] << 8 ) );
					row[x] = PixelFromGrayWord<pixeltype>(
						ApplyGrayWindow( v, header.black, header.white ) );
				}
				break;
			case libCZI::PixelType::Gray32Float:
				for (auto x = 0; x < w; ++x) {
					auto v = 0.0f;
					std::memcpy( &v, src + (size_t)x * 4, sizeof( float ) );
					row[x] = PixelFromGrayWord<pixeltype>( (uint16_t)std::clamp(
						v * 65535.0f, 0.0f, 65535.0f ) );
				}
				break;
			default:
				return false;
		}
	}
	return true;
}


template <typename pixeltype>
static bool DecodeCziSubBlock ( const uint8_t *bf, size_t len,
	const TileDst<pixeltype> &dst, TileDiag &diag )
{
	auto header = CziTileHeader{};
	if (!ParseCziTileHeader( bf, len, &header )) {
		LogDecodeFailure( ImgCodec::CziSubBlock, diag, bf, len,
			"malformed CZI tile descriptor" );
		return false;
	}

	const auto *payload = bf + kCziTileHeaderBytes;
	auto payload_len = len - (size_t)kCziTileHeaderBytes;

	try {
		auto bitmap = std::shared_ptr<libCZI::IBitmapData>{};
		switch (header.compression) {
			case libCZI::CompressionMode::UnCompressed: {
//
// Already pixels - read them straight out of the payload. The stride is whatever
// the stored rows actually occupy, which for a sub-block is the packed width.
				auto bpp = (size_t)libCZI::Utils::GetBytesPerPixel(
					header.pixel_type );
				auto stride = bpp * (size_t)header.w;
				if (bpp == 0 || payload_len < stride * (size_t)header.h) {
					LogDecodeFailure( ImgCodec::CziSubBlock, diag, bf, len,
						"uncompressed CZI sub-block is short" );
					return false;
				}
				return CopyCziPixels( payload, stride, header.pixel_type,
					header.w, header.h, header, dst );
			}
			case libCZI::CompressionMode::JpgXr:
			case libCZI::CompressionMode::Zstd0:
			case libCZI::CompressionMode::Zstd1: {
				auto type = header.compression == libCZI::CompressionMode::JpgXr
					? libCZI::ImageDecoderType::JPXR_JxrLib
					: ( header.compression == libCZI::CompressionMode::Zstd0
						? libCZI::ImageDecoderType::ZStd0
						: libCZI::ImageDecoderType::ZStd1 );
//
// The public way to reach a decoder: libCZI::GetSite() is an internal header.
// Default is the bundled JXRLib, which is what the /MD rebuild links; the
// WithWICDecoder alternative would hand JPEG XR to Windows Imaging Component.
				auto *site = libCZI::GetDefaultSiteObject(
					libCZI::SiteObjectType::Default );
				auto decoder = site
					? site->GetDecoder( type, nullptr )
					: std::shared_ptr<libCZI::IDecoder>{};
				if (!decoder) {
					LogDecodeFailure( ImgCodec::CziSubBlock, diag, bf, len,
						"libCZI has no decoder for this compression mode" );
					return false;
				}
				bitmap = decoder->Decode( payload, payload_len,
					header.pixel_type, (uint32_t)header.w, (uint32_t)header.h );
				break;
			}
			case libCZI::CompressionMode::Jpg:
				return DecodeJpeg( payload, payload_len, dst, diag );
			default:
				LogDecodeFailure( ImgCodec::CziSubBlock, diag, bf, len,
					"unsupported CZI compression mode" );
				return false;
		}

		if (!bitmap) {
			LogDecodeFailure( ImgCodec::CziSubBlock, diag, bf, len,
				"CZI sub-block produced no bitmap" );
			return false;
		}

		auto size = bitmap->GetSize();
		auto locker = libCZI::ScopedBitmapLockerSP{ bitmap };
		if (!CopyCziPixels( (const uint8_t *)locker.ptrDataRoi,
				(size_t)locker.stride, bitmap->GetPixelType(),
				(int)size.w, (int)size.h, header, dst ))
		{
			LogDecodeFailure( ImgCodec::CziSubBlock, diag, bf, len,
				"unsupported CZI pixel type" );
			return false;
		}
		return true;
	}
	catch (const std::exception &e) {
//
// libCZI reports every failure by exception, and one bad tile must not take the
// render down with it.
		LogDecodeFailure( ImgCodec::CziSubBlock, diag, bf, len, e.what() );
		return false;
	}
}

#endif // USE_CZI


// --- raw samples ---

size_t raw_sample_pixel_bytes ( RawSampleLayout layout ) noexcept
{
	switch (layout) {
		case RawSampleLayout::Gray8: return 1;
		case RawSampleLayout::Gray16: return 2;
		case RawSampleLayout::Rgb8: return 3;
		case RawSampleLayout::Rgb16: return 6;
	}
	return 0;
}


void write_raw_tile_header ( uint8_t *dest, RawSampleLayout layout, int w, int h )
{
	const auto width = (uint32_t)w;
	const auto height = (uint32_t)h;
	const auto reserved = uint16_t{ 0 };
	dest[0] = 'R';  dest[1] = 'A';  dest[2] = 'W';  dest[3] = 'S';
	dest[4] = kRawTileHeaderVersion;
	dest[5] = (uint8_t)layout;
	std::memcpy( dest + 6, &reserved, sizeof( reserved ) );
	std::memcpy( dest + 8, &width, sizeof( width ) );
	std::memcpy( dest + 12, &height, sizeof( height ) );
}


// The pixels are decoded already; only the conversion to the requested type is
// left, by the same widening and narrowing rules every other codec here applies.
template <typename pixeltype>
static bool DecodeRawSamples ( const uint8_t *bf, size_t len,
	const TileDst<pixeltype> &dst, TileDiag &diag )
{
	if (len < (size_t)kRawTileHeaderBytes || bf[4] != kRawTileHeaderVersion) {
		LogDecodeFailure( ImgCodec::RawSamples, diag, bf, len,
			"malformed raw-sample descriptor" );
		return false;
	}
	const auto layout = (RawSampleLayout)bf[5];
	auto w = uint32_t{ 0 };
	auto h = uint32_t{ 0 };
	std::memcpy( &w, bf + 8, sizeof( w ) );
	std::memcpy( &h, bf + 12, sizeof( h ) );
	const auto pixel_bytes = raw_sample_pixel_bytes( layout );
	if (pixel_bytes == 0 || w == 0 || h == 0 ||
		len - (size_t)kRawTileHeaderBytes < (size_t)w * h * pixel_bytes)
	{
		LogDecodeFailure( ImgCodec::RawSamples, diag, bf, len,
			"raw-sample payload is short or has an unknown layout" );
		return false;
	}

	auto stride = 0;
	auto *out = dst.reserve( (int)w, (int)h, stride );
	if (!out)
		return false;

	const auto *payload = bf + kRawTileHeaderBytes;
	const auto row_bytes = (size_t)w * pixel_bytes;
	for (auto y = 0u; y < h; ++y) {
		const auto *src = payload + (size_t)y * row_bytes;
		auto *row = out + (size_t)y * (size_t)stride;
		switch (layout) {
			case RawSampleLayout::Gray8:
				for (auto x = 0u; x < w; ++x)
					row[x] = PixelFromGrayByte<pixeltype>( src[x] );
				break;
			case RawSampleLayout::Gray16:
				for (auto x = 0u; x < w; ++x) {
					auto v = uint16_t{ 0 };
					std::memcpy( &v, src + (size_t)x * 2, sizeof( v ) );
					row[x] = PixelFromGrayWord<pixeltype>( v );
				}
				break;
			case RawSampleLayout::Rgb8:
				for (auto x = 0u; x < w; ++x, src += 3)
					row[x] = PixelFromRgbBytes<pixeltype>( src[0], src[1], src[2] );
				break;
			case RawSampleLayout::Rgb16:
				for (auto x = 0u; x < w; ++x, src += 6) {
					uint16_t rgb[3];
					std::memcpy( rgb, src, sizeof( rgb ) );
					row[x] = PixelFromRgbWords<pixeltype>( rgb[0], rgb[1], rgb[2] );
				}
				break;
		}
	}
	return true;
}


// --- dispatch ---

template <typename pixeltype>
bool decode_tile ( const uint8_t *bf, size_t len, ColorHint hint,
	const TileDst<pixeltype> &dst, TileDiag &diag )
{
	auto t0 = std::chrono::steady_clock::now();
	diag.decode_ms = 0.0;
#if !USE_OPENJPEG
	(void)hint;								// only the JPEG 2000 path consults it
#endif
	if (!bf || len == 0 || !dst.reserve) {
		LogDecodeFailure( ImgCodec::None, diag, bf, len, "empty tile buffer" );
		return false;
	}

	auto codec = sniff_img_codec( bf, len );
	auto ok = false;
	switch (codec) {
		case ImgCodec::Jpeg:
			ok = DecodeJpeg( bf, len, dst, diag );
			break;
		case ImgCodec::Jp2k:
#if USE_OPENJPEG
			ok = DecodeJp2k( bf, len, hint, dst, diag );
#else
			LogDecodeFailure( codec, diag, bf, len, "built without USE_OPENJPEG" );
#endif
			break;
		case ImgCodec::Jxl:
#if USE_JXL
			ok = DecodeJxl( bf, len, dst, diag );
#else
			LogDecodeFailure( codec, diag, bf, len, "built without USE_JXL" );
#endif
			break;
		case ImgCodec::CziSubBlock:
#if USE_CZI
			ok = DecodeCziSubBlock( bf, len, dst, diag );
#else
			LogDecodeFailure( codec, diag, bf, len, "built without USE_CZI" );
#endif
			break;
		case ImgCodec::RawSamples:
			ok = DecodeRawSamples( bf, len, dst, diag );
			break;
		case ImgCodec::Tiff: {
			auto error = std::string{};
			ok = tiff_image_decode( bf, len, dst, &error );
			if (!ok)
				LogDecodeFailure( codec, diag, bf, len, error.c_str() );
			break;
		}
		default:
// Names the format so an unsupported slide reports what it actually contains,
// rather than a generic decode failure.
			LogDecodeFailure( codec, diag, bf, len,
				codec == ImgCodec::None ? "unrecognized encoding"
					: "no decoder built for this codec" );
			break;
	}

	diag.decode_ms = ElapsedMs( t0 );
	return ok;
}


template <typename pixeltype>
bool decode_to_image ( const uint8_t *bf, size_t len, Pwhl<pixeltype> &out )
{
	auto dst = TileDst<pixeltype>{};
	dst.reserve = [&out] ( int w, int h, int &stride ) -> pixeltype * {
		if (w <= 0 || h <= 0)
			return nullptr;
		out = Pwhl<pixeltype>( Dim2D<int>{ w, h } );
		if (!out.p)
			return nullptr;
		stride = out.l;
		return out.p;
	};
	auto diag = TileDiag{};
	return decode_tile( bf, len, ColorHint::FromStream, dst, diag );
}


bool jxl_read_info ( const uint8_t *bf, size_t len, JxlImageInfo *out,
	std::string *error )
{
	if (error)
		error->clear();
	if (!out)
		return SetJxlError( error, "JPEG XL info output is null" );
#if USE_JXL
	auto info = JxlBasicInfo{};
	if (!ReadJxlBasicInfo( bf, len, &info, error ))
		return false;
	out->width = (int)info.xsize;
	out->height = (int)info.ysize;
	out->color_channels = (int)info.num_color_channels;
	out->bits_per_sample = (int)info.bits_per_sample;
//
// libjxl signals a floating-point sample format by a non-zero exponent width,
// not by a type tag; this is the only place that reading has to happen.
	out->is_float = info.exponent_bits_per_sample != 0;
	return true;
#else
	(void)bf;
	(void)len;
	return SetJxlError( error, "built without USE_JXL" );
#endif
}


bool read_img_codec_info ( const uint8_t *bf, size_t len, ImgCodecInfo *out,
	std::string *error )
{
	if (error)
		error->clear();
	if (!out)
		return SetCodecError( error, "codec info output is null" );
	*out = {};
	const auto codec = sniff_img_codec( bf, len );
	switch (codec) {
	case ImgCodec::Jpeg:
		return ReadJpegInfo( bf, len, out, error );
	case ImgCodec::Jp2k:
#if USE_OPENJPEG
		return ReadJp2kInfo( bf, len, out, error );
#else
		return SetCodecError( error, "built without USE_OPENJPEG" );
#endif
	case ImgCodec::Jxl: {
		auto info = JxlImageInfo{};
		if (!jxl_read_info( bf, len, &info, error ))
			return false;
		out->codec = ImgCodec::Jxl;
		out->width = info.width;
		out->height = info.height;
		out->color_channels = info.color_channels;
		out->bits_per_sample = info.bits_per_sample;
		out->is_float = info.is_float;
		return true;
	}
	default:
		return SetCodecError( error, codec == ImgCodec::None
			? "unrecognized image signature"
			: std::string( "unsupported image codec: " ) + img_codec_name( codec ) );
	}
}


template <typename pixeltype>
bool jxl_decode ( const uint8_t *bf, size_t len, Pwhl<pixeltype> *out,
	std::string *error )
{
	if (error)
		error->clear();
#if USE_JXL
	return DecodeJxlOwning( bf, len, out, error );
#else
	(void)bf;
	(void)len;
	(void)out;
	return SetJxlError( error, "built without USE_JXL" );
#endif
}


template <typename pixeltype>
bool jxl_encode ( const Pwhl<pixeltype> &image, std::vector<uint8_t> *out,
	std::string *error, const TileEncodeOptions &options )
{
	if (error)
		error->clear();
#if USE_JXL
	return EncodeJxlTyped( image, out, options, error );
#else
	(void)image;
	(void)out;
	(void)options;
	return SetJxlError( error, "built without USE_JXL" );
#endif
}


template <typename pixeltype>
bool encode_tile ( ImgCodec codec, const Pwhl<pixeltype> &image,
	std::vector<uint8_t> *out, const TileEncodeOptions &options,
	std::string *error )
{
	if (error)
		error->clear();
	if (!out)
		return SetCodecError( error, "tile encode output buffer is null" );
	out->clear();

	switch (codec) {
	case ImgCodec::Jpeg:
		return EncodeJpeg( image, out, options, error );
	case ImgCodec::Jp2k:
#if USE_OPENJPEG
		return EncodeJp2k( image, out, options, error );
#else
		return SetCodecError( error, "built without USE_OPENJPEG" );
#endif
	case ImgCodec::Jxl:
		return jxl_encode( image, out, error, options );
	default:
		break;
	}
//
// Named, not just refused: a writer that reaches here asked for an encoding this
// build cannot produce, and which one it was is the whole diagnosis.
	auto message = std::string( "no encoder for codec " ) + img_codec_name( codec );
	pyr::plog_err( "img_codec: %s\n", message.c_str() );
	return SetCodecError( error, message );
}


template bool decode_tile<Rgb> ( const uint8_t *, size_t, ColorHint,
	const TileDst<Rgb> &, TileDiag & );
template bool decode_tile<uint8_t> ( const uint8_t *, size_t, ColorHint,
	const TileDst<uint8_t> &, TileDiag & );
template bool decode_tile<uint16_t> ( const uint8_t *, size_t, ColorHint,
	const TileDst<uint16_t> &, TileDiag & );
template bool decode_tile<float> ( const uint8_t *, size_t, ColorHint,
	const TileDst<float> &, TileDiag & );
template bool decode_tile<Rgb16> ( const uint8_t *, size_t, ColorHint,
	const TileDst<Rgb16> &, TileDiag & );

template bool decode_to_image<Rgb16> ( const uint8_t *, size_t, Pwhl<Rgb16> & );
template bool decode_to_image<Rgb> ( const uint8_t *, size_t, Pwhl<Rgb> & );
template bool decode_to_image<uint8_t> ( const uint8_t *, size_t, Pwhl<uint8_t> & );
template bool decode_to_image<uint16_t> ( const uint8_t *, size_t, Pwhl<uint16_t> & );
template bool decode_to_image<float> ( const uint8_t *, size_t, Pwhl<float> & );

template bool jxl_decode<Rgb> ( const uint8_t *, size_t, Pwhl<Rgb> *, std::string * );
template bool jxl_decode<uint8_t> ( const uint8_t *, size_t, Pwhl<uint8_t> *, std::string * );
template bool jxl_decode<uint16_t> ( const uint8_t *, size_t, Pwhl<uint16_t> *, std::string * );
template bool jxl_decode<float> ( const uint8_t *, size_t, Pwhl<float> *, std::string * );
template bool jxl_decode<Rgb16> ( const uint8_t *, size_t, Pwhl<Rgb16> *, std::string * );

template bool jxl_encode<Rgb> ( const Pwhl<Rgb> &, std::vector<uint8_t> *,
	std::string *, const TileEncodeOptions & );
template bool jxl_encode<uint8_t> ( const Pwhl<uint8_t> &, std::vector<uint8_t> *,
	std::string *, const TileEncodeOptions & );
template bool jxl_encode<uint16_t> ( const Pwhl<uint16_t> &, std::vector<uint8_t> *,
	std::string *, const TileEncodeOptions & );
template bool jxl_encode<float> ( const Pwhl<float> &, std::vector<uint8_t> *,
	std::string *, const TileEncodeOptions & );
template bool jxl_encode<Rgb16> ( const Pwhl<Rgb16> &, std::vector<uint8_t> *,
	std::string *, const TileEncodeOptions & );

template bool encode_tile<Rgb> ( ImgCodec, const Pwhl<Rgb> &,
	std::vector<uint8_t> *, const TileEncodeOptions &, std::string * );
template bool encode_tile<uint8_t> ( ImgCodec, const Pwhl<uint8_t> &,
	std::vector<uint8_t> *, const TileEncodeOptions &, std::string * );
template bool encode_tile<uint16_t> ( ImgCodec, const Pwhl<uint16_t> &,
	std::vector<uint8_t> *, const TileEncodeOptions &, std::string * );
template bool encode_tile<float> ( ImgCodec, const Pwhl<float> &,
	std::vector<uint8_t> *, const TileEncodeOptions &, std::string * );
template bool encode_tile<Rgb16> ( ImgCodec, const Pwhl<Rgb16> &,
	std::vector<uint8_t> *, const TileEncodeOptions &, std::string * );

}
