// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "img_htj2k.h"
#include "img_htj2k_quantization.h"

#if USE_OPENJPH
#include <openjph/ojph_codestream.h>
#include <openjph/ojph_file.h>
#include <openjph/ojph_mem.h>
#include <openjph/ojph_message.h>
#include <openjph/ojph_params.h>

#if PYR_MSVC_AUTOLINK
#ifdef _DEBUG
#pragma comment(lib, "openjph.0.33d.lib")
#else
#pragma comment(lib, "openjph.0.33.lib")
#endif
#endif
#endif

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>

namespace priml {

namespace {

struct Htj2kView {
	const uint8_t *data = nullptr;
	size_t size = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	unsigned channels = 0;
	unsigned bits = 0;
	unsigned color = 0;
	bool supported_color = true;
};


uint32_t ReadBe32 ( const uint8_t *p ) noexcept
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}


struct Jp2Box {
	uint32_t type = 0;
	const uint8_t *data = nullptr;
	size_t size = 0;
};


bool ReadJp2Box ( const uint8_t *bf, size_t len, size_t &pos, Jp2Box &box ) noexcept
{
	if (len - pos < 8)
		return false;
	const auto *start = bf + pos;
	auto length = (uint64_t)ReadBe32( start );
	auto header = size_t{ 8 };
	if (length == 1) {
		if (len - pos < 16)
			return false;
		length = (uint64_t)ReadBe32( start + 8 ) << 32 | ReadBe32( start + 12 );
		header = 16;
	} else if (length == 0) {
		length = len - pos;
	}
	if (length < header || length > len - pos)
		return false;
	box = { ReadBe32( start + 4 ), start + header, (size_t)length - header };
	pos += (size_t)length;
	return true;
}


Htj2kView Htj2kInputView ( const uint8_t *bf, size_t len ) noexcept
{
	if (!bf)
		return {};
	if (len >= 4 && ReadBe32( bf ) == 0xff4fff51u)
		return { bf, len };
// JP2 and JPH share the signature box. Find the bounded jp2c box, and retain
// declared geometry/colour so decoding cannot silently ignore conflicting metadata.
	if (len < 12 || ReadBe32( bf ) != 12 || ReadBe32( bf + 4 ) != 0x6a502020u ||
		ReadBe32( bf + 8 ) != 0x0d0a870au)
		return {};
	auto view = Htj2kView{};
	auto pos = size_t{ 12 };
	while (pos < len) {
		auto box = Jp2Box{};
		if (!ReadJp2Box( bf, len, pos, box ))
			return {};
		if (box.type == 0x6a703263u) {
			if (view.data)
				return {};
			view.data = box.data;
			view.size = box.size;
		} else if (box.type == 0x6a703268u) {
			auto child_pos = size_t{};
			while (child_pos < box.size) {
				auto child = Jp2Box{};
				if (!ReadJp2Box( box.data, box.size, child_pos, child ))
					return {};
				if (child.type == 0x69686472u) {
					if (child.size != 14 || view.width != 0)
						return {};
					view.height = ReadBe32( child.data );
					view.width = ReadBe32( child.data + 4 );
					view.channels = (unsigned)child.data[8] << 8 | child.data[9];
					view.bits = child.data[10] == 255 ? 0u : (unsigned)child.data[10] + 1;
					if (!view.width || !view.height || !view.channels || child.data[11] != 7)
						return {};
				} else if (child.type == 0x636f6c72u) {
					const auto color = child.size >= 7 ? ReadBe32( child.data + 3 ) : 0;
					view.supported_color &= child.size == 7 && child.data[0] == 1 && (color == 16 || color == 17);
					view.color = color;
				} else if (child.type == 0x63646566u || child.type == 0x70636c72u || child.type == 0x636d6170u) {
// Alpha/channel remapping and palettes require a container conversion stage.
					view.supported_color = false;
				}
			}
		}
	}
	return view;
}

} // namespace


bool is_htj2k_codestream ( const uint8_t *bf, size_t len ) noexcept
{
// SOC, SIZ, Lsiz, Rsiz: Part 15 requires Rsiz bit 14. Do not search compressed
// packet bytes for markers, or a classic JPEG 2000 stream can be misidentified.
	const auto view = Htj2kInputView( bf, len );
	return view.data && view.size >= 8 && ReadBe32( view.data ) == 0xff4fff51u &&
		(view.data[6] & 0x40) != 0;
}


static bool Htj2kError ( std::string *error, const std::string &message )
{
	if (error)
		*error = "HTJ2K: " + message;
	return false;
}


#if USE_OPENJPH

// OpenJPH's default exception only says "ojph error". Install a handler once,
// before creating any streams, to retain the actual diagnostic in the exception.
// No per-operation mutation of the library's global message configuration.
class Htj2kMessages final : public ojph::message_error {
public:
	void operator() ( int code, const char *, int, const char *fmt, ... ) override
	{
		char message[2048];
		va_list args;
		va_start( args, fmt );
		std::vsnprintf( message, sizeof( message ), fmt, args );
		va_end( args );
		throw std::runtime_error( std::format( "OpenJPH {:08x}: {}", code, message ) );
	}
};


static void InitHtj2kMessages ()
{
	static auto messages = Htj2kMessages{};
	static auto once = std::once_flag{};
	std::call_once( once, [] { ojph::configure_error( &messages ); } );
}


static bool ReadHtj2kHeader ( ojph::codestream &stream, ojph::mem_infile &input,
	const uint8_t *bf, size_t len, ImgCodecInfo *out, std::string *error )
{
	if (!is_htj2k_codestream( bf, len ))
		return Htj2kError( error, "expected an HTJ2K codestream or JP2/JPH container" );
	const auto view = Htj2kInputView( bf, len );
	if (!view.supported_color)
		return Htj2kError( error, "unsupported JP2/JPH colour profile or channel mapping" );
	input.open( view.data, view.size );
	stream.read_headers( &input );
	const auto siz = stream.access_siz();
	const auto extent = siz.get_image_extent();
	const auto offset = siz.get_image_offset();
	const auto count = siz.get_num_components();
	if (extent.x <= offset.x || extent.y <= offset.y ||
		extent.x - offset.x > (unsigned)std::numeric_limits<int>::max() ||
		extent.y - offset.y > (unsigned)std::numeric_limits<int>::max())
		return Htj2kError( error, "invalid image dimensions" );
	if (count != 1 && count != 3)
		return Htj2kError( error, "only one or three components are supported" );
	const auto bits = siz.get_bit_depth( 0 );
	if (bits == 0 || bits > 16)
		return Htj2kError( error, "only integer samples up to 16 bits are supported" );
	for (auto c = 0u; c < count; ++c) {
		const auto sampling = siz.get_downsampling( c );
		if (siz.is_signed( c ) || siz.get_bit_depth( c ) != bits ||
			sampling.x != 1 || sampling.y != 1)
			return Htj2kError( error, "requires unsigned components of equal depth without subsampling" );
	}
	*out = { ImgCodec::Htj2k, (int)(extent.x - offset.x),
		(int)(extent.y - offset.y), (int)count, (int)bits, false };
	if (view.width && (view.width != (unsigned)out->width || view.height != (unsigned)out->height ||
		view.channels != count || (view.bits && view.bits != bits)))
		return Htj2kError( error, "JP2/JPH header conflicts with the codestream" );
	if ((view.color == 16 && count != 3) || (view.color == 17 && count != 1))
		return Htj2kError( error, "JP2/JPH colour declaration conflicts with the components" );
	return true;
}


template <typename pixeltype>
static pixeltype Htj2kPixel ( int r, int g, int b, int bits )
{
	constexpr auto word_output = std::is_same_v<pixeltype, uint16_t> ||
		std::is_same_v<pixeltype, Rgb16>;
	constexpr auto byte_output = std::is_same_v<pixeltype, uint8_t> ||
		std::is_same_v<pixeltype, Rgb>;
	const auto maximum = (1 << bits) - 1;
	auto sample = [=] ( int value ) -> uint16_t {
		value = std::clamp( value, 0, maximum );
		if constexpr (!word_output) {
			if (bits > 8)
				value >>= bits - 8;
			else if (bits < 8 && !byte_output)
				value = (value * 255 + maximum / 2) / maximum;
		}
		return (uint16_t)value;
	};
	const auto a = sample( r );
	const auto c = sample( g );
	const auto d = sample( b );
	if constexpr (word_output) {
		if (a == c && a == d)
			return PixelFromGrayWord<pixeltype>( a );
		return PixelFromRgbWords<pixeltype>( a, c, d );
	} else {
		if (a == c && a == d)
			return PixelFromGrayByte<pixeltype>( (uint8_t)a );
		return PixelFromRgbBytes<pixeltype>( (uint8_t)a, (uint8_t)c, (uint8_t)d );
	}
}


template <typename pixeltype>
static bool DecodeHtj2k ( ojph::codestream &stream, const ImgCodecInfo &info,
	const TileDst<pixeltype> &dst, std::string *error )
{
	if (!dst.reserve)
		return Htj2kError( error, "destination callback is empty" );
	auto stride = info.width;
	auto *pixels = dst.reserve( info.width, info.height, stride );
	if (!pixels || stride < info.width ||
		(size_t)info.height > std::numeric_limits<size_t>::max() / sizeof( pixeltype ) / (size_t)stride)
		return Htj2kError( error, "invalid destination buffer or stride" );
	stream.set_planar( false );
	stream.create();
// Keep just one row per component. The codec owns its wavelet/codeblock scratch;
// the adapter does not allocate an additional full-image surface per tile worker.
	auto rows = std::array<ojph::line_buf *, 3>{};
	for (auto y = 0; y < info.height; ++y) {
		for (auto c = 0; c < info.color_channels; ++c) {
			auto component = 0u;
			rows[c] = stream.pull( component );
			if (!rows[c] || !rows[c]->i32 || component != (unsigned)c || rows[c]->size < (size_t)info.width)
				return Htj2kError( error, "unexpected component row" );
		}
		auto *row = pixels + (size_t)y * stride;
		for (auto x = 0; x < info.width; ++x) {
			const auto r = rows[0]->i32[x];
			const auto g = info.color_channels == 1 ? r : rows[1]->i32[x];
			const auto b = info.color_channels == 1 ? r : rows[2]->i32[x];
			row[x] = Htj2kPixel<pixeltype>( r, g, b, info.bits_per_sample );
		}
	}
	return true;
}


template <typename pixeltype>
static int Htj2kSample ( const pixeltype &pixel, unsigned component )
{
	if constexpr (is_rgb_pixel_v<pixeltype>)
		return component == 0 ? pixel.r : component == 1 ? pixel.g : pixel.b;
	else
		return (int)pixel;
}


template <typename pixeltype>
static bool EncodeHtj2k ( const Pwhl<pixeltype> &image, std::vector<uint8_t> *out,
	const TileEncodeOptions &options, std::string *error )
{
	if constexpr (std::is_same_v<pixeltype, float>) {
		return Htj2kError( error, "float encoding is unsupported" );
	} else {
		constexpr auto channels = is_rgb_pixel_v<pixeltype> ? 3u : 1u;
		constexpr auto container_bits = (int)(sizeof( channel_type<pixeltype> ) * 8);
		const auto bits = options.bits_per_sample == 0 ? container_bits : options.bits_per_sample;
		if (!image.p || image.w <= 0 || image.h <= 0 || image.l < image.w)
			return Htj2kError( error, "invalid input raster" );
		if (bits < 1 || bits > container_bits)
			return Htj2kError( error, "invalid declared sample depth" );
		if (options.quality < kImageQualityMin || options.quality > kImageLosslessQuality)
			return Htj2kError( error, "quality must be 1..100 or 101 for lossless" );
		const auto maximum = (1 << bits) - 1;
		if (options.max_pixel_value < 0 || options.max_pixel_value > maximum)
			return Htj2kError( error, "quality reference range exceeds declared sample depth" );
		for (auto y = 0; y < image.h; ++y)
			for (auto x = 0; x < image.w; ++x)
				for (auto c = 0u; c < channels; ++c)
					if (Htj2kSample( image.rowp( y )[x], c ) > maximum)
						return Htj2kError( error, "sample exceeds declared depth" );

		auto output = ojph::mem_outfile{};
		auto stream = ojph::codestream{};
		auto siz = stream.access_siz();
		siz.set_image_extent( ojph::point( image.w, image.h ) );
		siz.set_image_offset( ojph::point( 0, 0 ) );
		siz.set_tile_size( ojph::size( image.w, image.h ) );
		siz.set_tile_offset( ojph::point( 0, 0 ) );
		siz.set_num_components( channels );
		for (auto c = 0u; c < channels; ++c)
			siz.set_component( c, ojph::point( 1, 1 ), bits, false );
		auto cod = stream.access_cod();
		auto decompositions = 0u;
		for (auto edge = std::min( image.w, image.h ); edge >= 2 && decompositions < detail::kHtj2kMaxDecompositions; edge /= 2)
			++decompositions;
// OpenJPH 0.33.0's zero-decomposition encoder loses the most negative centred
// sample (unsigned zero becomes midscale). Its own CLI reproduces this for a
// 1x1 zero or a 4x1 [0,64,128,255] raster. One level works for singleton axes
// and preserves all sample values; keep every stream we write off that path.
		constexpr auto min_decompositions = 1u;
		decompositions = std::max( decompositions, min_decompositions );
		cod.set_num_decomposition( decompositions );
		cod.set_color_transform( channels == 3 );
		cod.set_reversible( options.quality == kImageLosslessQuality );
		if (options.quality != kImageLosslessQuality) {
			if (!options.max_pixel_value || options.max_pixel_value == maximum)
				stream.access_qcd().set_qfactor( (float)options.quality );
			else {
				for (auto c = 0u; c < channels; ++c) {
					const auto quant = detail::htj2k_range_quantization(
						options.quality, bits, options.max_pixel_value, c, decompositions );
					if (c == 0)
						stream.access_qcd().set_irrev_quant( quant.delta,
							3 * decompositions + 1, quant.weights.data() );
					else
						stream.access_qcd().set_irrev_quant( c, quant.delta,
							3 * decompositions + 1, quant.weights.data() );
				}
			}
		}
		stream.set_planar( false );
		output.open();
		stream.write_headers( &output );
		auto component = 0u;
		auto *line = stream.exchange( nullptr, component );
		for (auto y = 0; y < image.h; ++y) {
			for (auto c = 0u; c < channels; ++c) {
				if (!line || !line->i32 || component != c || line->size < (size_t)image.w)
					return Htj2kError( error, "unexpected encoder component row" );
				for (auto x = 0; x < image.w; ++x)
					line->i32[x] = Htj2kSample( image.rowp( y )[x], c );
				line = stream.exchange( line, component );
			}
		}
		stream.flush();
		out->assign( output.get_data(), output.get_data() + output.get_used_size() );
		return true;
	}
}

#endif


bool htj2k_read_info ( const uint8_t *bf, size_t len, ImgCodecInfo *out,
	std::string *error )
{
	if (error)
		error->clear();
	if (!out)
		return Htj2kError( error, "info output is null" );
	*out = {};
#if USE_OPENJPH
	try {
		InitHtj2kMessages();
		auto input = ojph::mem_infile{};
		auto stream = ojph::codestream{};
		return ReadHtj2kHeader( stream, input, bf, len, out, error );
	} catch (const std::exception &e) {
		return Htj2kError( error, e.what() );
	}
#else
	(void)bf;
	(void)len;
	return Htj2kError( error, "built without USE_OPENJPH" );
#endif
}


template <typename pixeltype>
bool htj2k_decode ( const uint8_t *bf, size_t len, ColorHint hint,
	const TileDst<pixeltype> &dst, std::string *error )
{
	if (error)
		error->clear();
#if USE_OPENJPH
	try {
		InitHtj2kMessages();
		auto input = ojph::mem_infile{};
		auto stream = ojph::codestream{};
		auto info = ImgCodecInfo{};
		if (!ReadHtj2kHeader( stream, input, bf, len, &info, error ))
			return false;
		if (hint == ColorHint::YCbCr && info.color_channels == 3)
			return Htj2kError( error, "external YCbCr conversion is unsupported" );
		const auto view = Htj2kInputView( bf, len );
		auto end = view.size;
// Encapsulated frames can have one zero byte to make the fragment length even.
		if (end >= 3 && end % 2 == 0 && view.data[end - 1] == 0)
			--end;
		if (end < 2 || view.data[end - 2] != 0xff || view.data[end - 1] != 0xd9)
			return Htj2kError( error, "missing end-of-codestream marker" );
		return DecodeHtj2k( stream, info, dst, error );
	} catch (const std::exception &e) {
		return Htj2kError( error, e.what() );
	}
#else
	(void)bf;
	(void)len;
	(void)hint;
	(void)dst;
	return Htj2kError( error, "built without USE_OPENJPH" );
#endif
}


template <typename pixeltype>
bool htj2k_encode ( const Pwhl<pixeltype> &image, std::vector<uint8_t> *out,
	const TileEncodeOptions &options, std::string *error )
{
	if (error)
		error->clear();
	if (!out)
		return Htj2kError( error, "output buffer is null" );
	out->clear();
#if USE_OPENJPH
	try {
		InitHtj2kMessages();
		return EncodeHtj2k( image, out, options, error );
	} catch (const std::exception &e) {
		out->clear();
		return Htj2kError( error, e.what() );
	}
#else
	(void)image;
	(void)options;
	return Htj2kError( error, "built without USE_OPENJPH" );
#endif
}


#define INSTANTIATE_HTJ2K(T) \
	template bool htj2k_decode<T> ( const uint8_t *, size_t, ColorHint, const TileDst<T> &, std::string * ); \
	template bool htj2k_encode<T> ( const Pwhl<T> &, std::vector<uint8_t> *, const TileEncodeOptions &, std::string * );

INSTANTIATE_HTJ2K( uint8_t )
INSTANTIATE_HTJ2K( uint16_t )
INSTANTIATE_HTJ2K( Rgb )
INSTANTIATE_HTJ2K( Rgb16 )
INSTANTIATE_HTJ2K( float )

#undef INSTANTIATE_HTJ2K

}
