// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// PyrA's own CZI sub-block decoders (img_czi: bundled jxrlib and zstd) must give
// exactly the pixels libCZI's decoders give. Built only with PYR_WITH_CZI, since
// libCZI is the reference.
//
// Usage: pyra_czi_decode_test [SLIDE.czi ...]
// Synthetic streams cover every pixel type and both zstd flavours; SLIDE
// arguments add the compressed sub-blocks of real files.

#include "img_czi.h"

#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable : 4251 4244 4458 4864 )
#endif
#include <libCZI.h>
#ifdef _MSC_VER
#pragma warning( pop )
#endif

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr auto kSyntheticWidth = 301;
constexpr auto kSyntheticHeight = 77;
// Sub-blocks compared per real slide: enough to meet every pyramid layer.
constexpr auto kMaxSubBlocksPerSlide = 48;

void require ( bool condition, const std::string &message )
{
	if (!condition)
		throw std::runtime_error( message );
}


double elapsed_ms ( std::chrono::steady_clock::time_point start )
{
	return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
}


// libCZI's decode of `data`, as packed rows.
std::vector<uint8_t> libczi_decode ( libCZI::ImageDecoderType decoder_type, const void *data, size_t size,
	libCZI::PixelType type, int width, int height )
{
	auto *site = libCZI::GetDefaultSiteObject( libCZI::SiteObjectType::Default );
	require( site != nullptr, "libCZI site is unavailable" );
	auto decoder = site->GetDecoder( decoder_type, nullptr );
	require( decoder != nullptr, "libCZI has no such decoder" );
	auto bitmap = decoder->Decode( data, size, type, (uint32_t)width, (uint32_t)height );
	require( bitmap != nullptr, "libCZI decoded nothing" );
	auto lock = libCZI::ScopedBitmapLockerSP{ bitmap };
	const auto row = (size_t)width * (size_t)libCZI::Utils::GetBytesPerPixel( type );
	auto packed = std::vector<uint8_t>( row * (size_t)height );
	for (auto y = 0; y < height; ++y)
		std::memcpy( packed.data() + row * y, static_cast<const uint8_t *>( lock.ptrDataRoi ) + (size_t)lock.stride * y, row );
	return packed;
}


std::vector<uint8_t> pattern ( libCZI::PixelType type, int width, int height )
{
	const auto bytes = (size_t)width * height * libCZI::Utils::GetBytesPerPixel( type );
	auto data = std::vector<uint8_t>( bytes );
	for (size_t i = 0; i < bytes; ++i)
		data[i] = uint8_t( ( i * 7 + ( i / 13 ) * 31 + ( i >> 9 ) ) & 0xff );
	return data;
}


priml::CziPixelType ours ( libCZI::PixelType type )
{
	return priml::CziPixelType( (uint8_t)type );
}


void test_czi_jpeg_xr_matches_libczi ()
{
	const auto start = std::chrono::steady_clock::now();
	auto checked = 0;
	for (const auto type : { libCZI::PixelType::Gray8, libCZI::PixelType::Gray16, libCZI::PixelType::Bgr24,
		libCZI::PixelType::Bgr48 })
	{
		const auto source = pattern( type, kSyntheticWidth, kSyntheticHeight );
		const auto stride = (uint32_t)( kSyntheticWidth * libCZI::Utils::GetBytesPerPixel( type ) );
		for (const auto quality : { 1000u, 600u }) {
			auto parameters = libCZI::CompressParametersOnMap{};
			parameters.map[(int)libCZI::CompressionParameterKey::JXRLIB_QUALITY] = libCZI::CompressParameter( quality );
			auto stream = libCZI::JxrLibCompress::Compress( type, kSyntheticWidth, kSyntheticHeight, stride,
				source.data(), &parameters );
			const auto *data = static_cast<const uint8_t *>( stream->GetPtr() );
			const auto expected = libczi_decode( libCZI::ImageDecoderType::JPXR_JxrLib, data, stream->GetSizeOfData(),
				type, kSyntheticWidth, kSyntheticHeight );
			auto actual = std::vector<uint8_t>{};
			auto error = std::string{};
			require( priml::czi_decode_jpeg_xr( data, stream->GetSizeOfData(), ours( type ), kSyntheticWidth,
				kSyntheticHeight, &actual, &error ), "JPEG XR decode failed: " + error );
			require( actual == expected, "JPEG XR pixels differ from libCZI for pixel type " +
				std::to_string( (int)type ) + " quality " + std::to_string( quality ) );
			++checked;
		}
	}
	std::cout << "test_czi_jpeg_xr_matches_libczi: PASS (" << checked << " streams, " << elapsed_ms( start ) << " ms)\n";
}


void test_czi_zstd_matches_libczi ()
{
	const auto start = std::chrono::steady_clock::now();
	auto checked = 0;
	for (const auto type : { libCZI::PixelType::Gray8, libCZI::PixelType::Gray16, libCZI::PixelType::Bgr24,
		libCZI::PixelType::Bgr48, libCZI::PixelType::Gray32Float })
	{
		const auto source = pattern( type, kSyntheticWidth, kSyntheticHeight );
		const auto stride = (uint32_t)( kSyntheticWidth * libCZI::Utils::GetBytesPerPixel( type ) );
		const auto check = [&] ( const std::shared_ptr<libCZI::IMemoryBlock> &stream, bool zstd1, const char *what ) {
			const auto *data = static_cast<const uint8_t *>( stream->GetPtr() );
			const auto expected = libczi_decode( zstd1 ? libCZI::ImageDecoderType::ZStd1 : libCZI::ImageDecoderType::ZStd0,
				data, stream->GetSizeOfData(), type, kSyntheticWidth, kSyntheticHeight );
			require( expected == source, std::string( "libCZI did not round-trip " ) + what );
			auto actual = std::vector<uint8_t>{};
			auto error = std::string{};
			require( priml::czi_decode_zstd( data, stream->GetSizeOfData(), zstd1, ours( type ), kSyntheticWidth,
				kSyntheticHeight, &actual, &error ), std::string( what ) + " decode failed: " + error );
			require( actual == expected, std::string( what ) + " pixels differ from libCZI for pixel type " +
				std::to_string( (int)type ) );
			++checked;
		};
		check( libCZI::ZstdCompress::CompressZStd0Alloc( kSyntheticWidth, kSyntheticHeight, stride, type,
			source.data(), nullptr ), false, "zstd0" );
		for (const auto split : { false, true }) {
			const auto sixteen = type == libCZI::PixelType::Gray16 || type == libCZI::PixelType::Bgr48;
			if (split && !sixteen)
				continue;
			auto parameters = libCZI::CompressParametersOnMap{};
			parameters.map[(int)libCZI::CompressionParameterKey::ZSTD_PREPROCESS_DOLOHIBYTEPACKING] =
				libCZI::CompressParameter( split );
			check( libCZI::ZstdCompress::CompressZStd1Alloc( kSyntheticWidth, kSyntheticHeight, stride, type,
				source.data(), &parameters ), true, split ? "zstd1 (byte split)" : "zstd1" );
		}
	}
	std::cout << "test_czi_zstd_matches_libczi: PASS (" << checked << " streams, " << elapsed_ms( start ) << " ms)\n";
}


void test_czi_subblocks_match_libczi ( const std::filesystem::path &path )
{
	const auto start = std::chrono::steady_clock::now();
	auto stream = libCZI::CreateStreamFromFile( path.wstring().c_str() );
	auto reader = libCZI::CreateCZIReader();
	reader->Open( stream );
	auto indices = std::vector<int>{};
	reader->EnumerateSubBlocks( [&] ( int index, const libCZI::SubBlockInfo &info ) {
		const auto mode = info.GetCompressionMode();
		if (mode == libCZI::CompressionMode::JpgXr || mode == libCZI::CompressionMode::Zstd0 ||
			mode == libCZI::CompressionMode::Zstd1)
			indices.push_back( index );
		return (int)indices.size() < kMaxSubBlocksPerSlide;
	} );
	for (const auto index : indices) {
		auto block = reader->ReadSubBlock( index );
		const auto &info = block->GetSubBlockInfo();
		const void *data = nullptr;
		auto size = size_t{ 0 };
		block->DangerousGetRawData( libCZI::ISubBlock::Data, data, size );
		const auto w = (int)info.physicalSize.w;
		const auto h = (int)info.physicalSize.h;
		const auto mode = info.GetCompressionMode();
		const auto decoder = mode == libCZI::CompressionMode::JpgXr ? libCZI::ImageDecoderType::JPXR_JxrLib :
			mode == libCZI::CompressionMode::Zstd0 ? libCZI::ImageDecoderType::ZStd0 : libCZI::ImageDecoderType::ZStd1;
		const auto expected = libczi_decode( decoder, data, size, info.pixelType, w, h );
		auto actual = std::vector<uint8_t>{};
		auto error = std::string{};
		const auto *bytes = static_cast<const uint8_t *>( data );
		const auto ok = mode == libCZI::CompressionMode::JpgXr
			? priml::czi_decode_jpeg_xr( bytes, size, ours( info.pixelType ), w, h, &actual, &error )
			: priml::czi_decode_zstd( bytes, size, mode == libCZI::CompressionMode::Zstd1, ours( info.pixelType ), w, h,
				&actual, &error );
		require( ok, path.filename().string() + " sub-block " + std::to_string( index ) + ": " + error );
		require( actual == expected, path.filename().string() + " sub-block " + std::to_string( index ) +
			" differs from libCZI" );
	}
	reader->Close();
	std::cout << "test_czi_subblocks_match_libczi " << path.filename().string() << ": PASS (" << indices.size()
		<< " compressed sub-blocks, " << elapsed_ms( start ) << " ms)\n";
}

} // namespace


int main ( int argc, char **argv )
{
	try {
		test_czi_jpeg_xr_matches_libczi();
		test_czi_zstd_matches_libczi();
		for (auto i = 1; i < argc; ++i)
			test_czi_subblocks_match_libczi( std::filesystem::path( argv[i] ) );
		std::cout << "CZI sub-block decoders match libCZI: PASS\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
