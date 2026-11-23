// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "img_codec.h"

#include <pyra/pyra.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr auto kTileEdge = 256;
constexpr auto kPlanes = 5;
using Bytes = std::vector<uint8_t>;
using Entries = std::vector<std::pair<std::string, std::string>>;

void require ( bool condition, const char *message )
{
	if (!condition) throw std::runtime_error( message );
}


void integer ( Bytes &bytes, uint64_t value, int count )
{
	for (auto i = 0; i < count; ++i) bytes.push_back( uint8_t( value >> ( i * 8 ) ) );
}


void string ( Bytes &bytes, const std::string &text )
{
	require( text.size() < 255, "Fixture string exceeds short CString encoding" );
	bytes.insert( bytes.end(), { 0xff, 0xfe, 0xff, uint8_t( text.size() ) } );
	for (const auto c : text) { bytes.push_back( uint8_t( c ) ); bytes.push_back( 0 ); }
}


void group ( Bytes &bytes, const std::string &key, const Entries &entries )
{
	string( bytes, key );
	integer( bytes, entries.size(), 4 );
	for (const auto &[name, value] : entries) { string( bytes, name ); string( bytes, value ); }
}


void digests ( Bytes &bytes )
{
	string( bytes, "iKUOzef/dvVvwvRJLqHEOw==" );
	string( bytes, "Z2RRDfhj4eEAZ9QXgJjq8Q==" );
}


void write_fixture ( const std::filesystem::path &path )
{
	auto bytes = Bytes{};
	auto index = Bytes{};
	digests( index );
	for (auto plane = 0; plane < kPlanes; ++plane) {
		for (auto tile = 0; tile < 3; ++tile) {
			auto image = priml::Pwhl<priml::Rgb>( { kTileEdge, kTileEdge } );
			for (auto y = 0; y < image.h; ++y) for (auto x = 0; x < image.w; ++x) {
				const auto value = uint8_t( tile == 1 ? 255 : 40 + plane * 35 + ( ( x ^ y ) & 1 ) * 4 );
				image.rowp( y )[x] = { value, value, value };
			}
			auto jpeg = Bytes{};
			auto options = priml::TileEncodeOptions{};
			options.quality = 90;
			require( priml::encode_tile( priml::ImgCodec::Jpeg, image, &jpeg, options ), "Fixture JPEG encode failed" );
			group( index, std::format( "0,{},{},{},0", plane * 2, tile == 2 ? 1 : 0, tile == 1 ? 1 : 0 ),
				{ { "StartPosition", std::to_string( bytes.size() ) }, { "DataLength", std::to_string( jpeg.size() ) } } );
			bytes.insert( bytes.end(), jpeg.begin(), jpeg.end() );
		}
	}
	group( bytes, "Preview", {} );
	group( bytes, "SliceInfo", { { "MicrometersPerPixel", "0.25" }, { "ROILeft", "0" },
		{ "ROITop", "0" }, { "ROIRight", "1" }, { "ROIBottom", "1" }, { "FlatNum", "5" }, { "MLZStep", "20" } } );
	const auto index_offset = bytes.size();
	bytes.insert( bytes.end(), index.begin(), index.end() );
	const auto fov_offset = bytes.size();
	digests( bytes );
	const auto third_offset = bytes.size();
	digests( bytes );
	const auto fourth_offset = bytes.size();
	digests( bytes );
	for (const auto offset : { index_offset, fov_offset, third_offset, fourth_offset }) integer( bytes, offset, 8 );
	const auto version_offset = bytes.size();
	string( bytes, "2.01" );
	integer( bytes, version_offset, 8 );
	auto output = std::ofstream( path, std::ios::binary );
	output.write( reinterpret_cast<const char *>( bytes.data() ), std::streamsize( bytes.size() ) );
	require( output.good(), "Fixture ZYP write failed" );
}


struct Opened {
	pyra_context *context_ = nullptr;
	pyra_wsi *wsi_ = nullptr;
	~Opened () { if (wsi_) pyra_close( wsi_, nullptr ); if (context_) pyra_context_destroy( context_, nullptr ); }
};


void test_wsi_zyp_all_focal_planes_render ( const std::filesystem::path &path )
{
	auto opened = Opened{};
	auto error = pyra_error{};
	require( pyra_context_create( nullptr, &opened.context_, &error ) == PYRA_OK, error.message );
	const auto utf8 = path.u8string();
	require( pyra_open( opened.context_, reinterpret_cast<const char *>( utf8.c_str() ), &opened.wsi_, &error ) == PYRA_OK,
		error.message );
	auto count = uint32_t{};
	require( pyra_get_plane_count( opened.wsi_, &count, &error ) == PYRA_OK && count == kPlanes,
		"Five-plane ZYP does not expose all focal planes; slider would be hidden" );
	for (auto i = 0u; i < count; ++i) {
		auto info = pyra_plane_info{ sizeof( pyra_plane_info ) };
		require( pyra_get_plane_info( opened.wsi_, i, &info, &error ) == PYRA_OK &&
			std::isnan( info.z_um ) && info.level_count == 2, "ZYP invented physical Z or mixed resolution ladders" );
		auto request = pyra_image_request{ sizeof( pyra_image_request ), i, { 0, 0, 4, 4 }, 0.25 };
		auto layout = pyra_image_layout{ sizeof( pyra_image_layout ) };
		require( pyra_get_image_layout( opened.wsi_, &request, &layout, &error ) == PYRA_OK, error.message );
		auto pixels = Bytes( layout.capacity_bytes );
		auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format, pixels.data(), pixels.size(), layout.row_stride_bytes };
		require( pyra_get_image( opened.wsi_, &request, &buffer, &error ) == PYRA_OK, error.message );
		require( std::abs( int( pixels[0] ) - ( 40 + int( i ) * 35 ) ) <= 6, "ZYP rendered a different focal plane" );
	}
}

} // namespace


int main ( int argc, char **argv )
{
	if (argc != 2) return 2;
	try {
		const auto directory = std::filesystem::path( argv[1] );
		std::filesystem::create_directories( directory );
		const auto path = directory / "five-plane.zyp";
		write_fixture( path );
		const auto start = std::chrono::steady_clock::now();
		test_wsi_zyp_all_focal_planes_render( path );
		std::cout << "test_wsi_zyp_all_focal_planes_render: PASS ("
			<< std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::steady_clock::now() - start ).count() << " us)\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "test_wsi_zyp_all_focal_planes_render: FAIL: " << error.what() << '\n';
		return 1;
	}
}
