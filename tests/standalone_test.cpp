#include "img_codec.h"
#include "ptf_write.h"
#include "tiff_inc.h"
#include "wsi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
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


template <typename Pixel>
void write_color_fixture ( const std::filesystem::path &output,
	const char *name, priml::ImgCodec codec, Pixel value, int range )
{
	auto writer = priml::PtfWriter{};
	auto error = std::string{};
	require( writer.open( ( output / name ).string(), &error ), "Color fixture open: " + error );
	auto spec = priml::PtfLevelSpec{};
	spec.tags.valid = true;
	spec.tags.mpp = 1;
	spec.tags.pixel_type = priml::pixel_type_of<Pixel>::value;
	spec.tags.max_pixel_value = range;
	spec.size = { kTestEdge, kTestEdge };
	spec.tile = spec.size;
	spec.codec = codec;
	spec.encode.quality = priml::kImageDefaultLossyQuality;
	if (codec == priml::ImgCodec::Jp2k)
		spec.encode.quality = priml::kImageLosslessQuality;
	auto fetch = priml::PtfTileFetch<Pixel>(
		[value] ( const priml::Rtlbr<int> &, priml::Pwhl<Pixel> *tile ) {
			std::fill( tile->buf.begin(), tile->buf.end(), value );
			return true;
		} );
	require( writer.add_level( spec, fetch, &error ), "Color fixture level: " + error );
	require( writer.close( &error ), "Color fixture close: " + error );
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
	passed &= run_case( "write_unsupported_codec_fixture", [&] {
		const auto output = std::filesystem::path( argv[1] );
		const auto path = output / "unsupported.ptf";
		std::filesystem::copy_file( output / "standalone.ptf", path, std::filesystem::copy_options::overwrite_existing );
		auto handle = priml::TiffHandle{};
		require( handle.open( path.string() ), "Cannot read unsupported-codec fixture directory" );
		const auto offset = TIFFGetStrileOffset( static_cast<TIFF *>( handle.get() ), 0 );
		handle.close();
		auto stream = std::fstream( path, std::ios::in | std::ios::out | std::ios::binary );
		const unsigned char jpeg_ls[] = { 0xff, 0xd8, 0xff, 0xf7 };
		stream.seekp( (std::streamoff)offset );
		stream.write( reinterpret_cast<const char *>( jpeg_ls ), sizeof( jpeg_ls ) );
		require( bool( stream ), "Cannot write unsupported JPEG-LS tile signature" );
	} );
	passed &= run_case( "write_c_api_coarse_fixture", [&] {
		auto writer = priml::PtfWriter{};
		auto error = std::string{};
		require( writer.open( ( std::filesystem::path( argv[1] ) / "coarse.ptf" ).string(), &error ), error );
		auto spec = priml::PtfLevelSpec{};
		spec.size = { 2048, 2048 };
		spec.tile = { 256, 256 };
		spec.tags.valid = true;
		spec.tags.mpp = 1;
		spec.tags.pixel_type = priml::LevelPixelType::Gray8;
		spec.codec = priml::ImgCodec::Jpeg;
		spec.encode.quality = priml::kImageDefaultLossyQuality;
		auto fetch = priml::PtfTileFetch<uint8_t>(
			[] ( const priml::Rtlbr<int> &, priml::Pwhl<uint8_t> *tile ) {
				std::fill( tile->buf.begin(), tile->buf.end(), kGrayValue );
				return true;
			} );
		require( writer.add_level( spec, fetch, &error ), error );
		require( writer.close( &error ), error );
	} );
	passed &= run_case( "write_c_api_color_fixtures", [&] {
		const auto output = std::filesystem::path( argv[1] );
		write_color_fixture( output, "rgb8.ptf", priml::ImgCodec::Jpeg, priml::Rgb8{ 37, 117, 220 }, 255 );
#if USE_OPENJPEG
		write_color_fixture( output, "rgb16.ptf", priml::ImgCodec::Jp2k, priml::Rgb16{ 511, 2048, 4095 }, 4095 );
#endif
	} );
	return passed ? 0 : 1;
}
