// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdio>

#include <jpeglib.h>
#include <httplib.h>
#include <openjpeg.h>
#include <openjph/ojph_codestream.h>
#include <tiffio.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr auto kEdge = 16;

void require ( bool condition, const char *message )
{
	if (!condition)
		throw std::runtime_error( message );
}


template <typename Sample>
void test_jpeg_lossless_round_trip ( int bits )
{
	auto source = std::vector<Sample>( kEdge * kEdge );
	for (size_t i = 0; i < source.size(); ++i)
		source[i] = Sample( ( i * 137 ) & ( ( 1u << bits ) - 1 ) );
	auto compressor = jpeg_compress_struct{};
	auto errors = jpeg_error_mgr{};
	compressor.err = jpeg_std_error( &errors );
	jpeg_create_compress( &compressor );
	auto *encoded = static_cast<unsigned char *>( nullptr );
	auto length = 0ul;
	jpeg_mem_dest( &compressor, &encoded, &length );
	compressor.image_width = compressor.image_height = kEdge;
	compressor.input_components = 1;
	compressor.in_color_space = JCS_GRAYSCALE;
	jpeg_set_defaults( &compressor );
	compressor.data_precision = bits;
	jpeg_enable_lossless( &compressor, 1, 0 );
	jpeg_start_compress( &compressor, TRUE );
	while (compressor.next_scanline < kEdge) {
		auto *row = source.data() + compressor.next_scanline * kEdge;
		if (bits == 8) {
			auto *samples = reinterpret_cast<JSAMPLE *>( row );
			jpeg_write_scanlines( &compressor, &samples, 1 );
		} else if (bits == 12) {
			auto *samples = reinterpret_cast<J12SAMPLE *>( row );
			jpeg12_write_scanlines( &compressor, &samples, 1 );
		} else {
			auto *samples = reinterpret_cast<J16SAMPLE *>( row );
			jpeg16_write_scanlines( &compressor, &samples, 1 );
		}
	}
	jpeg_finish_compress( &compressor );
	jpeg_destroy_compress( &compressor );
	auto decoder = jpeg_decompress_struct{};
	decoder.err = jpeg_std_error( &errors );
	jpeg_create_decompress( &decoder );
	jpeg_mem_src( &decoder, encoded, length );
	jpeg_read_header( &decoder, TRUE );
	jpeg_start_decompress( &decoder );
	auto pixels = std::vector<Sample>( source.size() );
	while (decoder.output_scanline < kEdge) {
		auto *row = pixels.data() + decoder.output_scanline * kEdge;
		if (bits == 8) {
			auto *samples = reinterpret_cast<JSAMPLE *>( row );
			jpeg_read_scanlines( &decoder, &samples, 1 );
		} else if (bits == 12) {
			auto *samples = reinterpret_cast<J12SAMPLE *>( row );
			jpeg12_read_scanlines( &decoder, &samples, 1 );
		} else {
			auto *samples = reinterpret_cast<J16SAMPLE *>( row );
			jpeg16_read_scanlines( &decoder, &samples, 1 );
		}
	}
	jpeg_finish_decompress( &decoder );
	jpeg_destroy_decompress( &decoder );
	std::free( encoded );
	require( pixels == source, "JPEG lossless samples differ" );
}


void test_tiff_round_trip ( const std::filesystem::path &path, uint16_t compression )
{
	auto *file = TIFFOpen( path.c_str(), "w" );
	require( file, "Cannot create TIFF fixture" );
	require( TIFFSetField( file, TIFFTAG_IMAGEWIDTH, kEdge ) &&
		TIFFSetField( file, TIFFTAG_IMAGELENGTH, kEdge ) &&
		TIFFSetField( file, TIFFTAG_BITSPERSAMPLE, 16 ) &&
		TIFFSetField( file, TIFFTAG_SAMPLESPERPIXEL, 1 ) &&
		TIFFSetField( file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG ) &&
		TIFFSetField( file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK ) &&
		TIFFSetField( file, TIFFTAG_COMPRESSION, compression ), "TIFF tags failed" );
	auto row = std::vector<uint16_t>( kEdge, 43123 );
	for (auto y = 0; y < kEdge; ++y)
		require( TIFFWriteScanline( file, row.data(), y ) == 1, "TIFF write failed" );
	TIFFClose( file );
	file = TIFFOpen( path.c_str(), "r" );
	require( file, "Cannot reopen TIFF fixture" );
	auto output = std::vector<uint16_t>( kEdge );
	for (auto y = 0; y < kEdge; ++y)
		require( TIFFReadScanline( file, output.data(), y ) == 1 && output == row, "TIFF samples differ" );
	TIFFClose( file );
}


void test_openjpeg_lossless_round_trip ( const std::filesystem::path &path )
{
	auto component = opj_image_cmptparm_t{};
	component.dx = component.dy = 1;
	component.w = component.h = kEdge;
	component.prec = 16;
	auto *image = opj_image_create( 1, &component, OPJ_CLRSPC_GRAY );
	require( image, "OpenJPEG image allocation failed" );
	image->x1 = image->y1 = kEdge;
	for (auto i = 0; i < kEdge * kEdge; ++i)
		image->comps[0].data[i] = i * 137;
	auto parameters = opj_cparameters_t{};
	opj_set_default_encoder_parameters( &parameters );
	parameters.tcp_numlayers = 1;
	parameters.cp_disto_alloc = 1;
	parameters.numresolution = 2;
	auto *codec = opj_create_compress( OPJ_CODEC_J2K );
	auto *stream = opj_stream_create_default_file_stream( path.c_str(), OPJ_FALSE );
	require( codec && stream && opj_setup_encoder( codec, &parameters, image ) &&
		opj_start_compress( codec, image, stream ) && opj_encode( codec, stream ) &&
		opj_end_compress( codec, stream ), "OpenJPEG encode failed" );
	opj_stream_destroy( stream );
	opj_destroy_codec( codec );
	auto decoder_parameters = opj_dparameters_t{};
	opj_set_default_decoder_parameters( &decoder_parameters );
	codec = opj_create_decompress( OPJ_CODEC_J2K );
	stream = opj_stream_create_default_file_stream( path.c_str(), OPJ_TRUE );
	auto *decoded = static_cast<opj_image_t *>( nullptr );
	require( codec && stream && opj_setup_decoder( codec, &decoder_parameters ) &&
		opj_read_header( stream, codec, &decoded ) && opj_decode( codec, stream, decoded ) &&
		opj_end_decompress( codec, stream ), "OpenJPEG decode failed" );
	require( decoded->numcomps == 1 && decoded->comps[0].w == kEdge && decoded->comps[0].h == kEdge,
		"OpenJPEG geometry differs" );
	for (auto i = 0; i < kEdge * kEdge; ++i)
		require( decoded->comps[0].data[i] == i * 137, "OpenJPEG samples differ" );
	opj_image_destroy( decoded );
	opj_image_destroy( image );
	opj_stream_destroy( stream );
	opj_destroy_codec( codec );
}

} // namespace


int main ( int argc, char **argv )
{
	if (argc != 2)
		return 2;
	const auto directory = std::filesystem::path( argv[1] );
	std::filesystem::create_directories( directory );
	auto failed = 0;
	const auto run = [&] ( const char *name, auto test ) {
		const auto start = std::chrono::steady_clock::now();
		auto passed = true;
		try { test(); }
		catch (const std::exception &error) {
			std::cerr << error.what() << '\n';
			passed = false;
			++failed;
		}
		std::cout << name << ": " << ( passed ? "PASS" : "FAIL" ) << " ("
			<< std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::steady_clock::now() - start ).count()
			<< " us)\n";
	};
	run( "test_jpeg_8_lossless_round_trip", [] { test_jpeg_lossless_round_trip<uint8_t>( 8 ); } );
	run( "test_jpeg_12_lossless_round_trip", [] { test_jpeg_lossless_round_trip<uint16_t>( 12 ); } );
	run( "test_jpeg_16_lossless_round_trip", [] { test_jpeg_lossless_round_trip<uint16_t>( 16 ); } );
	run( "test_tiff_lzw_round_trip", [&] { test_tiff_round_trip( directory / "lzw.tif", COMPRESSION_LZW ); } );
#if PYRA_TEST_ZLIB
	run( "test_tiff_deflate_round_trip", [&] { test_tiff_round_trip( directory / "deflate.tif", COMPRESSION_ADOBE_DEFLATE ); } );
#endif
	run( "test_openjpeg_lossless_round_trip", [&] { test_openjpeg_lossless_round_trip( directory / "lossless.j2k" ); } );
	run( "test_openjph_initialization", [] { auto stream = ojph::codestream{}; } );
	run( "test_httplib_initialization", [] { auto server = httplib::Server{}; } );
	return failed ? 1 : 0;
}
