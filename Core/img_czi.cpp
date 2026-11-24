// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "img_czi.h"

#if USE_JXR
#include <JXRGlue.h>
#endif
#if USE_ZSTD
#include <zstd.h>
#endif

#include <algorithm>
#include <cstring>
#include <memory>

namespace priml {

namespace {

bool fail ( std::string *error, const char *message )
{
	if (error)
		*error = message;
	return false;
}


// The packed size of a sub-block, or 0 when it would overflow or is empty.
size_t packed_bytes ( CziPixelType type, int width, int height )
{
	const auto pixel = czi_pixel_bytes( type );
	if (pixel == 0 || width <= 0 || height <= 0)
		return 0;
	const auto count = (size_t)width * (size_t)height;
	if (count > SIZE_MAX / pixel)
		return 0;
	return count * pixel;
}

} // namespace


size_t czi_pixel_bytes ( CziPixelType type ) noexcept
{
	switch (type) {
		case CziPixelType::Gray8: return 1;
		case CziPixelType::Gray16: return 2;
		case CziPixelType::Gray32Float: return 4;
		case CziPixelType::Bgr24: return 3;
		case CziPixelType::Bgr48: return 6;
	}
	return 0;
}


#if USE_JXR

namespace {

struct JxrFormat {
	const PKPixelFormatGUID *guid;
	CziPixelType type;
	bool rgb_order;			// red first: swapped into CZI's blue-first order
};


// The codestream formats a CZI sub-block of each pixel type is written with.
const JxrFormat kJxrFormats[] = {
	{ &GUID_PKPixelFormat8bppGray, CziPixelType::Gray8, false },
	{ &GUID_PKPixelFormat16bppGray, CziPixelType::Gray16, false },
	{ &GUID_PKPixelFormat32bppGrayFloat, CziPixelType::Gray32Float, false },
	{ &GUID_PKPixelFormat24bppBGR, CziPixelType::Bgr24, false },
	{ &GUID_PKPixelFormat24bppRGB, CziPixelType::Bgr24, true },
	{ &GUID_PKPixelFormat48bppRGB, CziPixelType::Bgr48, true },
};


struct DecoderRelease {
	void operator() ( PKImageDecode *decoder ) const { decoder->Release( &decoder ); }
};

} // namespace


bool czi_decode_jpeg_xr ( const uint8_t *data, size_t size, CziPixelType type,
	int width, int height, std::vector<uint8_t> *pixels, std::string *error )
{
	const auto bytes = packed_bytes( type, width, height );
	if (!data || size == 0 || !pixels || bytes == 0)
		return fail( error, "invalid JPEG XR sub-block request" );
	struct tagWMPStream *stream = nullptr;
	if (CreateWS_Memory( &stream, const_cast<uint8_t *>( data ), size ) < 0 || !stream)
		return fail( error, "cannot wrap the JPEG XR payload in a stream" );
	PKImageDecode *raw = nullptr;
	if (PKImageDecode_Create_WMP( &raw ) < 0 || !raw) {
		stream->Close( &stream );
		return fail( error, "cannot create a JPEG XR decoder" );
	}
	auto decoder = std::unique_ptr<PKImageDecode, DecoderRelease>( raw );
	if (decoder->Initialize( decoder.get(), stream ) < 0) {
		stream->Close( &stream );
		return fail( error, "JPEG XR header is invalid" );
	}
// From here the decoder closes the stream when it is released.
	decoder->fStreamOwner = !0;
	auto format = PKPixelFormatGUID{};
	auto w = I32{ 0 };
	auto h = I32{ 0 };
	if (decoder->GetPixelFormat( decoder.get(), &format ) < 0 || decoder->GetSize( decoder.get(), &w, &h ) < 0)
		return fail( error, "JPEG XR image information is unavailable" );
	if (w != width || h != height)
		return fail( error, "JPEG XR image size differs from the sub-block" );
	const auto *match = std::find_if( std::begin( kJxrFormats ), std::end( kJxrFormats ),
		[&] ( const JxrFormat &candidate ) {
			return candidate.type == type && IsEqualGUID( candidate.guid, &format );
		} );
	if (match == std::end( kJxrFormats ))
		return fail( error, "JPEG XR pixel format does not match the sub-block pixel type" );
	const auto stride = (size_t)width * czi_pixel_bytes( type );
	pixels->assign( bytes, 0 );
	const auto rect = PKRect{ 0, 0, width, height };
	if (decoder->Copy( decoder.get(), &rect, pixels->data(), (U32)stride ) < 0)
		return fail( error, "JPEG XR decoding failed" );
	if (match->rgb_order) {
		const auto sample = czi_pixel_bytes( type ) / 3;
		for (size_t offset = 0; offset < bytes; offset += 3 * sample)
			std::swap_ranges( pixels->data() + offset, pixels->data() + offset + sample,
				pixels->data() + offset + 2 * sample );
	}
	return true;
}

#else

bool czi_decode_jpeg_xr ( const uint8_t *, size_t, CziPixelType, int, int,
	std::vector<uint8_t> *, std::string *error )
{
	return fail( error, "JPEG XR decoding requires PYR_WITH_JXR" );
}

#endif


#if USE_ZSTD

namespace {

// Zstd1 header: its own size, then chunks. Size 1 holds no chunk; size 3 holds
// chunk type 1, whose low bit is the low/high byte split.
constexpr uint8_t kZstd1BareHeader = 1;
constexpr uint8_t kZstd1ChunkedHeader = 3;
constexpr uint8_t kZstd1ByteSplitChunk = 1;

} // namespace


bool czi_decode_zstd ( const uint8_t *data, size_t size, bool zstd1, CziPixelType type,
	int width, int height, std::vector<uint8_t> *pixels, std::string *error )
{
	const auto bytes = packed_bytes( type, width, height );
	if (!data || size == 0 || !pixels || bytes == 0)
		return fail( error, "invalid zstd sub-block request" );
	auto split = false;
	if (zstd1) {
		const auto header = data[0];
		if (header == kZstd1ChunkedHeader && size > kZstd1ChunkedHeader && data[1] == kZstd1ByteSplitChunk)
			split = ( data[2] & 1 ) != 0;
		else if (header != kZstd1BareHeader || size <= kZstd1BareHeader)
			return fail( error, "zstd1 sub-block header is invalid" );
		data += header;
		size -= header;
	}
	if (split && ( bytes % 2 ) != 0)
		return fail( error, "zstd1 byte split needs 16-bit samples" );
	auto decoded = std::vector<uint8_t>( bytes );
	const auto written = ZSTD_decompress( decoded.data(), decoded.size(), data, size );
	if (ZSTD_isError( written ) || written != bytes)
		return fail( error, "zstd sub-block did not decompress to its size" );
	if (!split) {
		*pixels = std::move( decoded );
		return true;
	}
	pixels->assign( bytes, 0 );
	const auto words = bytes / 2;
	for (size_t i = 0; i < words; ++i) {
		( *pixels )[2 * i] = decoded[i];
		( *pixels )[2 * i + 1] = decoded[words + i];
	}
	return true;
}

#else

bool czi_decode_zstd ( const uint8_t *, size_t, bool, CziPixelType, int, int,
	std::vector<uint8_t> *, std::string *error )
{
	return fail( error, "zstd decoding requires PYR_WITH_ZSTD" );
}

#endif

} // namespace priml
