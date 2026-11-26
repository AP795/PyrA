// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "img_jxr.h"

#if USE_JXR
#include <JXRGlue.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <type_traits>

namespace priml {

namespace {

// JPEG XR quantisation-parameter index: 1 is lossless; lossy quality 100 starts
// just above it and every quality step below costs kJxrQpPerQualityStep more.
constexpr int kJxrLosslessQp = 1;
constexpr int kJxrQpAtQuality100 = 2;
constexpr double kJxrQpPerQualityStep = 1.5;
// Output stream growth while encoding: a tile rarely exceeds its raw size.
constexpr size_t kJxrStreamGrowBytes = size_t{ 64 } * 1024;

bool fail ( std::string *error, const char *message )
{
	if (error)
		*error = message;
	return false;
}

} // namespace


size_t jxr_pixel_bytes ( JxrLayout layout ) noexcept
{
	switch (layout) {
		case JxrLayout::Gray8: return 1;
		case JxrLayout::Gray16: return 2;
		case JxrLayout::Gray32Float: return 4;
		case JxrLayout::Bgr24:
		case JxrLayout::Rgb24: return 3;
		case JxrLayout::Rgb48: return 6;
	}
	return 0;
}


int jxr_quality_to_qp ( int quality ) noexcept
{
	if (quality >= kImageLosslessQuality)
		return kJxrLosslessQp;
	const auto clamped = std::clamp( quality, kImageQualityMin, kImageLossyQualityMax );
	return kJxrQpAtQuality100 + (int)std::lround( ( kImageLossyQualityMax - clamped ) * kJxrQpPerQualityStep );
}


#if USE_JXR

namespace {

struct JxrFormat {
	const PKPixelFormatGUID *guid;
	JxrLayout layout;
};

const JxrFormat kJxrFormats[] = {
	{ &GUID_PKPixelFormat8bppGray, JxrLayout::Gray8 },
	{ &GUID_PKPixelFormat16bppGray, JxrLayout::Gray16 },
	{ &GUID_PKPixelFormat32bppGrayFloat, JxrLayout::Gray32Float },
	{ &GUID_PKPixelFormat24bppBGR, JxrLayout::Bgr24 },
	{ &GUID_PKPixelFormat24bppRGB, JxrLayout::Rgb24 },
	{ &GUID_PKPixelFormat48bppRGB, JxrLayout::Rgb48 },
};


struct DecoderRelease {
	void operator() ( PKImageDecode *decoder ) const { decoder->Release( &decoder ); }
};

struct EncoderRelease {
	void operator() ( PKImageEncode *encoder ) const { encoder->Release( &encoder ); }
};

using DecoderPtr = std::unique_ptr<PKImageDecode, DecoderRelease>;


// A decoder over `bf`, owning its stream; null with `error` set on failure.
DecoderPtr open_decoder ( const uint8_t *bf, size_t len, std::string *error )
{
	if (!bf || len == 0) {
		fail( error, "empty JPEG XR stream" );
		return {};
	}
	struct tagWMPStream *stream = nullptr;
	if (Failed( CreateWS_Memory( &stream, const_cast<uint8_t *>( bf ), len ) ) || !stream) {
		fail( error, "cannot wrap the JPEG XR stream" );
		return {};
	}
	PKImageDecode *raw = nullptr;
	if (Failed( PKImageDecode_Create_WMP( &raw ) ) || !raw) {
		stream->Close( &stream );
		fail( error, "cannot create a JPEG XR decoder" );
		return {};
	}
	auto decoder = DecoderPtr( raw );
	if (Failed( decoder->Initialize( decoder.get(), stream ) )) {
		stream->Close( &stream );
		fail( error, "JPEG XR header is invalid" );
		return {};
	}
// From here the decoder closes the stream when it is released.
	decoder->fStreamOwner = !0;
	return decoder;
}


bool image_layout ( PKImageDecode &decoder, JxrLayout *layout, int *width, int *height, std::string *error )
{
	auto format = PKPixelFormatGUID{};
	auto w = I32{ 0 };
	auto h = I32{ 0 };
	if (Failed( decoder.GetPixelFormat( &decoder, &format ) ) || Failed( decoder.GetSize( &decoder, &w, &h ) ) ||
		w <= 0 || h <= 0)
		return fail( error, "JPEG XR image information is unavailable" );
	const auto *match = std::find_if( std::begin( kJxrFormats ), std::end( kJxrFormats ),
		[&] ( const JxrFormat &candidate ) { return IsEqualGUID( candidate.guid, &format ); } );
	if (match == std::end( kJxrFormats ))
		return fail( error, "unsupported JPEG XR pixel format" );
	*layout = match->layout;
	*width = (int)w;
	*height = (int)h;
	return true;
}


// The encoder's pixel format and colour coding for a raster of `pixeltype`.
template <typename pixeltype>
const PKPixelFormatGUID &encoder_format ()
{
	if constexpr (std::is_same_v<pixeltype, Rgb8>)
		return GUID_PKPixelFormat24bppRGB;
	else if constexpr (std::is_same_v<pixeltype, Rgb16>)
		return GUID_PKPixelFormat48bppRGB;
	else if constexpr (std::is_same_v<pixeltype, uint16_t>)
		return GUID_PKPixelFormat16bppGray;
	else if constexpr (std::is_same_v<pixeltype, float>)
		return GUID_PKPixelFormat32bppGrayFloat;
	else
		return GUID_PKPixelFormat8bppGray;
}

} // namespace


bool jxr_read_info ( const uint8_t *bf, size_t len, ImgCodecInfo *out, std::string *error )
{
	if (!out)
		return fail( error, "codec info output is null" );
	auto decoder = open_decoder( bf, len, error );
	if (!decoder)
		return false;
	auto layout = JxrLayout::Gray8;
	*out = {};
	if (!image_layout( *decoder, &layout, &out->width, &out->height, error ))
		return false;
	out->codec = ImgCodec::JpegXr;
	const auto colour = layout == JxrLayout::Bgr24 || layout == JxrLayout::Rgb24 || layout == JxrLayout::Rgb48;
	out->color_channels = colour ? 3 : 1;
	out->bits_per_sample = layout == JxrLayout::Gray32Float ? 32
		: ( layout == JxrLayout::Gray16 || layout == JxrLayout::Rgb48 ) ? 16 : 8;
	out->is_float = layout == JxrLayout::Gray32Float;
	return true;
}


bool jxr_decode_packed ( const uint8_t *bf, size_t len, JxrPacked *out, std::string *error )
{
	if (!out)
		return fail( error, "JPEG XR output is null" );
	auto decoder = open_decoder( bf, len, error );
	if (!decoder || !image_layout( *decoder, &out->layout, &out->width, &out->height, error ))
		return false;
	const auto stride = (size_t)out->width * jxr_pixel_bytes( out->layout );
	out->pixels.assign( stride * (size_t)out->height, 0 );
	const auto rect = PKRect{ 0, 0, out->width, out->height };
	if (Failed( decoder->Copy( decoder.get(), &rect, out->pixels.data(), (U32)stride ) ))
		return fail( error, "JPEG XR decoding failed" );
	return true;
}


template <typename pixeltype>
bool jxr_encode ( const Pwhl<pixeltype> &image, std::vector<uint8_t> *out,
	const TileEncodeOptions &options, std::string *error )
{
	if (!out || !image.p || image.w <= 0 || image.h <= 0)
		return fail( error, "invalid JPEG XR encode request" );
	out->clear();
	const auto lossless = options.quality >= kImageLosslessQuality;
//
// JPEG XR codes float samples through a reduced mantissa: even the full 23 bits
// changed samples in test_img_jxr_lossless_round_trip. Lossy float is allowed.
	if (std::is_same_v<pixeltype, float> && lossless)
		return fail( error, "JPEG XR has no lossless mode for float samples" );
//
// jxrlib reads rows as given; a packed copy also drops a Pwhl's line padding.
	const auto row = (size_t)image.w * sizeof( pixeltype );
	auto packed = std::vector<uint8_t>( row * (size_t)image.h );
	for (auto y = 0; y < image.h; ++y)
		std::memcpy( packed.data() + row * (size_t)y, image.rowp( y ), row );

	auto params = CWMIStrCodecParam{};
	params.cfColorFormat = is_rgb_pixel_v<pixeltype> ? YUV_444 : Y_ONLY;
	params.bdBitDepth = BD_LONG;
	params.bfBitstreamFormat = SPATIAL;
	params.olOverlap = lossless ? OL_NONE : OL_ONE;
	params.sbSubband = SB_ALL;
	params.uiDefaultQPIndex = (U8)jxr_quality_to_qp( options.quality );

	struct tagWMPStream *stream = nullptr;
	if (Failed( CreateWS_HeapBackedWriteableStream( &stream, row * (size_t)image.h / 2 + kJxrStreamGrowBytes,
		kJxrStreamGrowBytes ) ) || !stream)
		return fail( error, "cannot create the JPEG XR output stream" );
	PKImageEncode *raw = nullptr;
	if (Failed( PKImageEncode_Create_WMP( &raw ) ) || !raw) {
		stream->Close( &stream );
		return fail( error, "cannot create a JPEG XR encoder" );
	}
// The encoder closes the stream when it is released, so the bytes are copied out
// before that.
	auto encoder = std::unique_ptr<PKImageEncode, EncoderRelease>( raw );
	if (Failed( encoder->Initialize( encoder.get(), stream, &params, sizeof( params ) ) )) {
		stream->Close( &stream );
		return fail( error, "JPEG XR encoder parameters were refused" );
	}
	if (Failed( encoder->SetPixelFormat( encoder.get(), encoder_format<pixeltype>() ) ) ||
		Failed( encoder->SetSize( encoder.get(), image.w, image.h ) ) ||
		Failed( encoder->WritePixels( encoder.get(), (U32)image.h, packed.data(), (U32)row ) ))
		return fail( error, "JPEG XR encoding failed" );
	void *buffer = nullptr;
	auto size = size_t{ 0 };
	GetWS_HeapBackedWriteableStreamBuffer( stream, &buffer, &size );
	if (!buffer || size == 0)
		return fail( error, "JPEG XR encoder produced nothing" );
	const auto *bytes = static_cast<const uint8_t *>( buffer );
	out->assign( bytes, bytes + size );
	return true;
}

#else

bool jxr_read_info ( const uint8_t *, size_t, ImgCodecInfo *, std::string *error )
{
	return fail( error, "JPEG XR requires PYR_WITH_JXR" );
}


bool jxr_decode_packed ( const uint8_t *, size_t, JxrPacked *, std::string *error )
{
	return fail( error, "JPEG XR requires PYR_WITH_JXR" );
}


template <typename pixeltype>
bool jxr_encode ( const Pwhl<pixeltype> &, std::vector<uint8_t> *, const TileEncodeOptions &, std::string *error )
{
	return fail( error, "JPEG XR requires PYR_WITH_JXR" );
}

#endif


template <typename pixeltype>
bool jxr_decode ( const uint8_t *bf, size_t len, const TileDst<pixeltype> &dst, std::string *error )
{
	auto packed = JxrPacked{};
	if (!jxr_decode_packed( bf, len, &packed, error ))
		return false;
	auto stride = 0;
	auto *out = dst.reserve( packed.width, packed.height, stride );
	if (!out)
		return fail( error, "no destination for the JPEG XR image" );
	const auto row_bytes = (size_t)packed.width * jxr_pixel_bytes( packed.layout );
	for (auto y = 0; y < packed.height; ++y) {
		const auto *src = packed.pixels.data() + (size_t)y * row_bytes;
		auto *row = out + (size_t)y * (size_t)stride;
		for (auto x = 0; x < packed.width; ++x) {
			switch (packed.layout) {
				case JxrLayout::Gray8:
					row[x] = PixelFromGrayByte<pixeltype>( src[x] );
					break;
				case JxrLayout::Gray16: {
					auto v = uint16_t{ 0 };
					std::memcpy( &v, src + (size_t)x * 2, sizeof( v ) );
					row[x] = PixelFromGrayWord<pixeltype>( v );
					break;
				}
				case JxrLayout::Gray32Float: {
					auto v = 0.0f;
					std::memcpy( &v, src + (size_t)x * 4, sizeof( v ) );
					if constexpr (std::is_same_v<pixeltype, float>)
						row[x] = v;
					else
						row[x] = PixelFromGrayWord<pixeltype>( (uint16_t)std::clamp( v * 65535.0f, 0.0f, 65535.0f ) );
					break;
				}
				case JxrLayout::Bgr24: {
					const auto *p = src + (size_t)x * 3;
					row[x] = PixelFromRgbBytes<pixeltype>( p[2], p[1], p[0] );
					break;
				}
				case JxrLayout::Rgb24: {
					const auto *p = src + (size_t)x * 3;
					row[x] = PixelFromRgbBytes<pixeltype>( p[0], p[1], p[2] );
					break;
				}
				case JxrLayout::Rgb48: {
					uint16_t rgb[3];
					std::memcpy( rgb, src + (size_t)x * 6, sizeof( rgb ) );
					row[x] = PixelFromRgbWords<pixeltype>( rgb[0], rgb[1], rgb[2] );
					break;
				}
			}
		}
	}
	return true;
}


#define PYR_JXR_INSTANTIATE( T ) \
	template bool jxr_decode<T> ( const uint8_t *, size_t, const TileDst<T> &, std::string * ); \
	template bool jxr_encode<T> ( const Pwhl<T> &, std::vector<uint8_t> *, const TileEncodeOptions &, std::string * );
PYR_JXR_INSTANTIATE( Rgb )
PYR_JXR_INSTANTIATE( Rgb16 )
PYR_JXR_INSTANTIATE( uint8_t )
PYR_JXR_INSTANTIATE( uint16_t )
PYR_JXR_INSTANTIATE( float )
#undef PYR_JXR_INSTANTIATE

} // namespace priml
