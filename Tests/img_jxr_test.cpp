// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// JPEG XR as an image and PTF tile codec (img_jxr.*). Hosted by
// pyra_standalone_test.

#include "img_codec.h"
#include "img_jxr.h"
#include "ptf_write.h"
#include "tiff_inc.h"
#include "wsi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

constexpr auto kTileEdge = 96;
constexpr auto kLevelWidth = 300;
constexpr auto kLevelHeight = 210;
// Mean absolute error a quality-90 tile of the test pattern may have, in 8-bit
// codes: a loose bound that catches a broken mapping, not a tuning target.
constexpr auto kLossy90MeanError = 4.0;
// Float pattern spans -0.25..0.75; a quality-95 tile may be off by this much.
constexpr auto kLossyFloatQuality = 95;
constexpr auto kLossyFloatError = 0.05f;

void require ( bool condition, const std::string &message )
{
	if (!condition)
		throw std::runtime_error( message );
}


// Detail at every scale, so a lossy codec has something to lose.
template <typename pixeltype>
pixeltype pattern ( int x, int y )
{
	const auto v = ( x * 5 + y * 3 + ( ( x >> 3 ) ^ ( y >> 3 ) ) * 17 ) & 0xff;
	if constexpr (std::is_same_v<pixeltype, priml::Rgb8>)
		return { uint8_t( v ), uint8_t( 255 - v ), uint8_t( ( x * y ) & 0xff ) };
	else if constexpr (std::is_same_v<pixeltype, priml::Rgb16>)
		return { uint16_t( v * 257 ), uint16_t( ( x * 97 ) & 0xffff ), uint16_t( ( y * 131 ) & 0xffff ) };
	else if constexpr (std::is_same_v<pixeltype, uint16_t>)
		return uint16_t( ( v << 4 ) | ( x & 15 ) );
	else if constexpr (std::is_same_v<pixeltype, float>)
		return float( v ) / 255.0f - 0.25f;
	else
		return uint8_t( v );
}


template <typename pixeltype>
priml::Pwhl<pixeltype> make_image ( int width, int height )
{
	auto image = priml::Pwhl<pixeltype>( { width, height } );
	for (auto y = 0; y < height; ++y)
		for (auto x = 0; x < width; ++x)
			image.rowp( y )[x] = pattern<pixeltype>( x, y );
	return image;
}


template <typename pixeltype>
bool same_pixel ( const pixeltype &a, const pixeltype &b )
{
	if constexpr (std::is_same_v<pixeltype, priml::Rgb8> || std::is_same_v<pixeltype, priml::Rgb16>)
		return a.r == b.r && a.g == b.g && a.b == b.b;
	else
		return a == b;
}


std::vector<uint8_t> encode ( const auto &image, int quality )
{
	auto options = priml::TileEncodeOptions{};
	options.quality = quality;
	auto bytes = std::vector<uint8_t>{};
	auto error = std::string{};
	require( priml::encode_tile( priml::ImgCodec::JpegXr, image, &bytes, options, &error ), "JPEG XR encode: " + error );
	return bytes;
}


template <typename pixeltype>
void lossless_round_trip ( const char *name )
{
	const auto source = make_image<pixeltype>( kTileEdge + 7, kTileEdge - 5 );
	const auto bytes = encode( source, priml::kImageLosslessQuality );
	require( priml::sniff_img_codec( bytes.data(), bytes.size() ) == priml::ImgCodec::JpegXr,
		std::string( name ) + ": stream not identified as JPEG XR" );
	auto decoded = priml::Pwhl<pixeltype>{};
	require( priml::decode_to_image( bytes.data(), bytes.size(), decoded ) && priml::are_wh_eq( decoded, source ),
		std::string( name ) + ": decode failed or changed the size" );
	for (auto y = 0; y < source.h; ++y)
		for (auto x = 0; x < source.w; ++x)
			require( same_pixel( decoded.rowp( y )[x], source.rowp( y )[x] ),
				std::string( name ) + ": lossless JPEG XR changed a sample" );
}


void test_img_jxr_lossless_round_trip ()
{
	lossless_round_trip<priml::Rgb8>( "Rgb8" );
	lossless_round_trip<priml::Rgb16>( "Rgb16" );
	lossless_round_trip<uint8_t>( "Gray8" );
	lossless_round_trip<uint16_t>( "Gray16" );
}


// Float samples have only a lossy mode: lossless is refused, lossy decodes close.
void test_img_jxr_float_lossy_only ()
{
	const auto source = make_image<float>( kTileEdge, kTileEdge );
	auto options = priml::TileEncodeOptions{};
	auto bytes = std::vector<uint8_t>{};
	require( !priml::encode_tile( priml::ImgCodec::JpegXr, source, &bytes, options ),
		"Lossless float JPEG XR was accepted" );
	bytes = encode( source, kLossyFloatQuality );
	auto decoded = priml::Pwhl<float>{};
	require( priml::decode_to_image( bytes.data(), bytes.size(), decoded ) && priml::are_wh_eq( decoded, source ),
		"Lossy float JPEG XR did not decode" );
	auto largest = 0.0f;
	for (auto y = 0; y < source.h; ++y)
		for (auto x = 0; x < source.w; ++x)
			largest = std::max( largest, std::abs( decoded.rowp( y )[x] - source.rowp( y )[x] ) );
	require( largest <= kLossyFloatError, "Lossy float JPEG XR error is " + std::to_string( largest ) );
}


void test_img_jxr_lossy_quality_order ()
{
	const auto source = make_image<priml::Rgb8>( kTileEdge, kTileEdge );
	const auto lossless = encode( source, priml::kImageLosslessQuality );
	const auto q90 = encode( source, 90 );
	const auto q50 = encode( source, 50 );
	require( q90.size() < lossless.size() && q50.size() < q90.size(),
		"JPEG XR sizes do not fall with quality: lossless " + std::to_string( lossless.size() ) + ", q90 " +
		std::to_string( q90.size() ) + ", q50 " + std::to_string( q50.size() ) );
	auto decoded = priml::Pwhl<priml::Rgb8>{};
	require( priml::decode_to_image( q90.data(), q90.size(), decoded ), "Lossy JPEG XR did not decode" );
	auto total = 0.0;
	for (auto y = 0; y < source.h; ++y)
		for (auto x = 0; x < source.w; ++x) {
			const auto &a = source.rowp( y )[x];
			const auto &b = decoded.rowp( y )[x];
			total += std::abs( a.r - b.r ) + std::abs( a.g - b.g ) + std::abs( a.b - b.b );
		}
	const auto mean = total / ( 3.0 * source.w * source.h );
	require( mean <= kLossy90MeanError, "Quality-90 JPEG XR error is " + std::to_string( mean ) );
}


void test_img_jxr_codec_info ()
{
	const auto source = make_image<uint16_t>( 77, 41 );
	const auto bytes = encode( source, priml::kImageLosslessQuality );
	auto info = priml::ImgCodecInfo{};
	auto error = std::string{};
	require( priml::read_img_codec_info( bytes.data(), bytes.size(), &info, &error ), "JPEG XR info: " + error );
	require( info.codec == priml::ImgCodec::JpegXr && info.width == 77 && info.height == 41 &&
		info.color_channels == 1 && info.bits_per_sample == 16 && !info.is_float, "JPEG XR info differs" );
	auto codec = priml::ImgCodec::None;
	require( priml::parse_img_codec_id( "jxr", &codec ) && codec == priml::ImgCodec::JpegXr &&
		priml::img_codec_id( codec ) == "jxr", "JPEG XR codec id does not round-trip" );
}


// A PTF level coded as JPEG XR: compression tag 22610, and the reader renders
// the written samples back unchanged.
void test_img_jxr_ptf_lossless_level ( const std::filesystem::path &output )
{
	const auto path = ( output / "jxr_gray16.ptf" ).string();
	auto error = std::string{};
	auto writer = priml::PtfWriter{};
	require( writer.open( path, &error ), "PTF open: " + error );
	auto spec = priml::PtfLevelSpec{};
	spec.tags.valid = true;
	spec.tags.mpp = 1.0;
	spec.tags.pixel_type = priml::LevelPixelType::Gray16;
	spec.tags.max_pixel_value = 4095;
	spec.size = { kLevelWidth, kLevelHeight };
	spec.tile = { kTileEdge, kTileEdge };
	spec.codec = priml::ImgCodec::JpegXr;
	spec.encode.quality = priml::kImageLosslessQuality;
	const auto fetch = priml::PtfTileFetch<uint16_t>( [] ( const priml::Rtlbr<int> &rect, priml::Pwhl<uint16_t> *tile ) {
		for (auto y = 0; y < tile->h; ++y)
			for (auto x = 0; x < tile->w; ++x)
				tile->rowp( y )[x] = pattern<uint16_t>( rect.tl.x + x, rect.tl.y + y );
		return true;
	} );
	require( writer.add_level( spec, fetch, &error ) && writer.close( &error ), "PTF write: " + error );

	auto handle = priml::TiffHandle{};
	require( handle.open( path ), "Cannot reopen the JPEG XR PTF" );
	auto compression = uint16_t{ 0 };
	TIFFGetFieldDefaulted( static_cast<TIFF *>( handle.get() ), TIFFTAG_COMPRESSION, &compression );
	handle.close();
	require( compression == priml::kTiffCompressionJpegXr, "PTF tiles are not tagged JPEG XR (22610)" );

	auto pyramid = priml::Ppyr{};
	auto loader = priml::WsiLoader::Create( path );
	require( loader && loader->OpenFile<uint16_t>( path, pyramid ), "PTF reader could not open the JPEG XR file" );
	const auto size = priml::Dim2D<int>{ kLevelWidth, kLevelHeight };
	auto rendered = loader->Generate<uint16_t>( pyramid, nullptr, 1.0f, pyramid.mrct, &size );
	loader->Close( pyramid );
	require( rendered.w == kLevelWidth && rendered.h == kLevelHeight, "JPEG XR PTF render size differs" );
	for (auto y = 0; y < rendered.h; ++y)
		for (auto x = 0; x < rendered.w; ++x)
			require( rendered.rowp( y )[x] == pattern<uint16_t>( x, y ), "JPEG XR PTF pixels differ from the written ones" );
}


bool run_case ( const char *name, auto &&test )
{
	const auto start = std::chrono::steady_clock::now();
	auto passed = true;
	try { test(); }
	catch (const std::exception &error) {
		std::cerr << name << ": " << error.what() << '\n';
		passed = false;
	}
	std::cout << name << ": " << ( passed ? "PASS" : "FAIL" ) << " ("
		<< std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count() << " ms)\n";
	return passed;
}

} // namespace


bool run_img_jxr_tests ( const std::filesystem::path &output )
{
	if (!priml::img_codec_can_encode( priml::ImgCodec::JpegXr )) {
		std::cout << "img_jxr tests: skipped (built without USE_JXR)\n";
		return true;
	}
	auto passed = run_case( "test_img_jxr_lossless_round_trip", test_img_jxr_lossless_round_trip );
	passed &= run_case( "test_img_jxr_float_lossy_only", test_img_jxr_float_lossy_only );
	passed &= run_case( "test_img_jxr_lossy_quality_order", test_img_jxr_lossy_quality_order );
	passed &= run_case( "test_img_jxr_codec_info", test_img_jxr_codec_info );
	passed &= run_case( "test_img_jxr_ptf_lossless_level", [&] { test_img_jxr_ptf_lossless_level( output ); } );
	return passed;
}
