#include "img_codec.h"
#include "ptf_write.h"
#include "wsi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr auto kTestEdge = 32;
constexpr auto kGrayValue = uint8_t{ 117 };
constexpr auto kJpegTolerance = 3;

void require ( bool condition, const std::string &message )
{
	if (!condition)
		throw std::runtime_error( message );
}


void test_img_codec_jpeg_round_trip ()
{
	auto source = priml::Pwhl<uint8_t>( { kTestEdge, kTestEdge } );
	std::fill( source.buf.begin(), source.buf.end(), kGrayValue );
	auto encoded = std::vector<uint8_t>{};
	auto error = std::string{};
	auto options = priml::TileEncodeOptions{};
	options.quality = priml::kImageDefaultLossyQuality;
	require( priml::encode_tile( priml::ImgCodec::Jpeg, source, &encoded,
		options, &error ), "JPEG encode: " + error );
	auto decoded = priml::Pwhl<uint8_t>{};
	require( priml::decode_to_image( encoded.data(), encoded.size(), decoded ),
		"JPEG decode failed" );
	require( decoded.w == kTestEdge && decoded.h == kTestEdge,
		"JPEG dimensions changed" );
	for (auto y = 0; y < decoded.h; ++y)
		for (auto x = 0; x < decoded.w; ++x)
			require( std::abs( int( decoded.rowp( y )[x] ) - kGrayValue ) <=
				kJpegTolerance, "JPEG constant image changed beyond tolerance" );
}


#if USE_OPENJPEG
void test_img_codec_jp2k_lossless_round_trip ()
{
	auto source = priml::Pwhl<uint16_t>( { kTestEdge, kTestEdge } );
	for (auto y = 0; y < source.h; ++y)
		for (auto x = 0; x < source.w; ++x)
			source.rowp( y )[x] = static_cast<uint16_t>( y * kTestEdge + x );
	auto encoded = std::vector<uint8_t>{};
	auto error = std::string{};
	require( priml::encode_tile( priml::ImgCodec::Jp2k, source, &encoded,
		{}, &error ), "JPEG 2000 encode: " + error );
	auto decoded = priml::Pwhl<uint16_t>{};
	require( priml::decode_to_image( encoded.data(), encoded.size(), decoded ),
		"JPEG 2000 decode failed" );
	require( decoded.w == source.w && decoded.h == source.h,
		"JPEG 2000 dimensions changed" );
	for (auto y = 0; y < source.h; ++y)
		for (auto x = 0; x < source.w; ++x)
			require( decoded.rowp( y )[x] == source.rowp( y )[x],
				"Lossless JPEG 2000 changed a sample" );
}
#endif


void test_ptf_write_render_round_trip ( const std::filesystem::path &output )
{
	const auto path = ( output / "standalone.ptf" ).string();
	auto error = std::string{};
	auto writer = priml::PtfWriter{};
	require( writer.open( path, &error ), "PTF open: " + error );
	auto spec = priml::PtfLevelSpec{};
	spec.tags.valid = true;
	spec.tags.mpp = 1.0;
	spec.tags.pixel_type = priml::LevelPixelType::Gray8;
	spec.tags.max_pixel_value = 255;
	spec.size = { kTestEdge, kTestEdge };
	spec.tile = spec.size;
	spec.codec = priml::ImgCodec::Jpeg;
	spec.encode.quality = priml::kImageDefaultLossyQuality;
	auto fetch = priml::PtfTileFetch<uint8_t>(
		[] ( const priml::Rtlbr<int> &, priml::Pwhl<uint8_t> *tile ) {
			std::fill( tile->buf.begin(), tile->buf.end(), kGrayValue );
			return true;
		} );
	require( writer.add_level( spec, fetch, &error ), "PTF write: " + error );
	require( writer.close( &error ), "PTF close: " + error );
	auto pyramid = priml::Ppyr{};
	auto loader = priml::WsiLoader::Create( path );
	require( loader && loader->OpenFile<uint8_t>( path, pyramid ),
		"PTF reader could not open generated file" );
	auto rendered = loader->Generate<uint8_t>( pyramid, nullptr, 1.0f,
		pyramid.mrct, &spec.size );
	loader->Close( pyramid );
	require( rendered.w == kTestEdge && rendered.h == kTestEdge,
		"PTF render dimensions changed" );
	for (auto y = 0; y < rendered.h; ++y)
		for (auto x = 0; x < rendered.w; ++x)
			require( std::abs( int( rendered.rowp( y )[x] ) - kGrayValue ) <=
				kJpegTolerance, "PTF rendered pixels changed beyond tolerance" );
}


template <typename Function>
bool run_case ( const char *name, Function function )
{
	const auto start = std::chrono::steady_clock::now();
	auto passed = true;
	try {
		function();
	} catch (const std::exception &error) {
		std::cerr << name << ": " << error.what() << '\n';
		passed = false;
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - start ).count();
	std::cout << name << ": " << ( passed ? "PASS" : "FAIL" )
		<< " (" << elapsed << " us)\n";
	return passed;
}

} // namespace


int main ( int argc, char **argv )
{
	if (argc != 2) {
		std::cerr << "Usage: pyra_standalone_test <output-directory>\n";
		return 1;
	}
	auto passed = run_case( "test_img_codec_jpeg_round_trip",
		test_img_codec_jpeg_round_trip );
#if USE_OPENJPEG
	passed &= run_case( "test_img_codec_jp2k_lossless_round_trip",
		test_img_codec_jp2k_lossless_round_trip );
#endif
	passed &= run_case( "test_ptf_write_render_round_trip", [&] {
		test_ptf_write_render_round_trip( std::filesystem::path( argv[1] ) );
	} );
	return passed ? 0 : 1;
}
