// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "ptf_subset.h"
#include "tiff_inc.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void require ( bool condition, const std::string &message )
{
	if (!condition)
		throw std::runtime_error( message );
}


std::filesystem::path fixture ( const std::filesystem::path &directory,
	priml::Dim2D<int> size = { 96, 64 }, bool deep = false, bool sparse = false )
{
	std::filesystem::create_directories( directory );
	const auto path = directory / "source.ptf";
	auto writer = priml::PtfWriter{};
	auto error = std::string{};
	require( writer.open( path.string(), &error ), error );
	for (auto plane = 0; plane < ( deep ? 2 : 1 ); ++plane) {
		auto spec = priml::PtfLevelSpec{};
		spec.tags.valid = true;
		spec.tags.mpp = 0.5;
		spec.tags.pixel_type = deep ? priml::LevelPixelType::Gray16 : priml::LevelPixelType::Gray8;
		spec.tags.max_pixel_value = deep ? 4095 : 255;
		spec.tags.channel = deep ? ( plane == 0 ? "R" : "G" ) : "";
		spec.tags.zlevel = deep ? 2.5f : 0;
		spec.size = size;
		spec.tile = { 32, 32 };
		spec.codec = deep ? priml::ImgCodec::Jp2k : priml::ImgCodec::Jpeg;
		spec.encode.quality = deep ? priml::kImageLosslessQuality : 90;
		const auto write = [&] ( auto pixel ) {
			using Pixel = decltype( pixel );
			auto fetch = priml::PtfTileFetch<Pixel>( [&] ( const priml::Rtlbr<int> &rect, priml::Pwhl<Pixel> *tile ) {
				if (sparse && rect.tl.x == 32)
					return false;
				for (auto y = 0; y < tile->h; ++y)
					for (auto x = 0; x < tile->w; ++x)
						tile->rowp( y )[x] = (Pixel)( 20 + rect.tl.x / 32 * 30 + plane * 100 + y );
				return true;
			} );
			return writer.add_level( spec, fetch, &error );
		};
		const auto written = deep ? write( uint16_t{} ) : write( uint8_t{} );
		require( written, error );
	}
	require( writer.close( &error ), error );
	return path;
}


void compare_base ( priml::WsiSession &source, const priml::PtfSubsetResult &saved )
{
	auto subset = priml::WsiSession{};
	require( subset.open( saved.path ), "cannot reopen exported subset" );
	require( subset.pyramid().planes.size() == source.pyramid().planes.size(), "planes lost" );
	for (const auto &stack : subset.pyramid().planes) {
		const auto source_stack = priml::find_plane( source.pyramid(), stack.plane );
		require( source_stack && stack.pixel_type == source_stack->pixel_type &&
			stack.max_pixel_value == source_stack->max_pixel_value, "plane type/range changed" );
		const auto &base = subset.pyramid().levels[stack.ladder.front()];
		const auto source_no = source_stack->ladder.front();
		const auto &original = source.pyramid().levels[source_no];
		require( base.metric == original.metric, "base calibration changed" );
		for (auto i = 0; i < (int)base.tiles.size(); ++i) {
			const auto &tile = base.tiles[i];
			const auto x = tile.lr.tl.x + (int)std::llround( saved.source_rect.tl.x / base.metric );
			const auto y = tile.lr.tl.y + (int)std::llround( saved.source_rect.tl.y / base.metric );
			const auto found = std::find_if( original.tiles.begin(), original.tiles.end(), [&] ( const auto &candidate ) {
				return candidate.lr.tl.x == x && candidate.lr.tl.y == y;
			} );
			require( found != original.tiles.end(), "export tile has no source counterpart" );
			auto expected = std::vector<uint8_t>{};
			auto actual = std::vector<uint8_t>{};
			auto error = std::string{};
			require( source.loader()->ReadEncodedTile( source.pyramid(), source_no,
				(int)std::distance( original.tiles.begin(), found ), &expected, &error ), error );
			require( subset.loader()->ReadEncodedTile( subset.pyramid(), stack.ladder.front(), i, &actual, &error ), error );
			require( actual == expected, "level-0 encoded bytes changed" );
		}
	}
}


void test_ptf_subset_complete_intersecting_tiles ( const std::filesystem::path &directory )
{
	auto source = priml::WsiSession{};
	require( source.open( fixture( directory ) ), "source open failed" );
	const auto decodes = source.pyramid().stats.decoded_tiles;
	auto saved = priml::PtfSubsetResult{};
	auto error = std::string{};
	require( priml::save_ptf_subset( source, { { 15.5, 1 }, { 17, 14 } }, &saved, &error ), error );
	require( saved.source_rect.tl.x == 0 && saved.source_rect.br.x == 32 &&
		saved.source_rect.br.y == 16, "intersecting tiles were cropped or missed" );
	require( source.pyramid().stats.decoded_tiles == decodes, "base copy decoded source pixels" );
	compare_base( source, saved );
}


//
// A render records the sniffed encoding on the level it decoded from, so a host
// can name the tile codec behind a frame. Nothing is decoded by the open itself.
void test_level_tile_codec_recorded_by_render ( const std::filesystem::path &directory )
{
	auto source = priml::WsiSession{};
	require( source.open( fixture( directory ) ), "source open failed" );
	auto &pyramid = source.pyramid();
	for (const auto &level : pyramid.levels)
		require( level.tile_codec == priml::ImgCodec::None, "codec known before any decode" );
	const auto dims = priml::Dim2D<int>{ 96, 64 };
	auto image = source.loader()->Generate<uint8_t>( pyramid, nullptr, 0.5f,
		{ { 0, 0 }, { 48, 32 } }, &dims );
	require( image.p && pyramid.stats.last_render_level >= 0, "render failed" );
	require( pyramid.levels[pyramid.stats.last_render_level].tile_codec == priml::ImgCodec::Jpeg,
		"rendered level does not record its JPEG tiles" );
}


void test_ptf_subset_half_open_boundary ( const std::filesystem::path &directory )
{
	auto source = priml::WsiSession{};
	require( source.open( fixture( directory ) ), "source open failed" );
	auto saved = priml::PtfSubsetResult{};
	auto error = std::string{};
	require( priml::save_ptf_subset( source, { { 16, 0 }, { 32, 16 } }, &saved, &error ), error );
	require( saved.source_rect.tl.x == 16 && saved.source_rect.br.x == 32 &&
		saved.stats.tiles_written == 1, "boundary-touching neighbours were included" );
	compare_base( source, saved );
}


void test_ptf_subset_partial_source_edge ( const std::filesystem::path &directory )
{
	auto source = priml::WsiSession{};
	require( source.open( fixture( directory, { 70, 47 } ) ), "source open failed" );
	auto saved = priml::PtfSubsetResult{};
	auto error = std::string{};
	require( priml::save_ptf_subset( source, { { 33, 20 }, { 40, 30 } }, &saved, &error ), error );
	require( saved.source_rect.tl.x == 32 && saved.source_rect.br.x == 35 &&
		saved.source_rect.br.y == 23.5, "source edge extent changed" );
	compare_base( source, saved );
}


void test_ptf_subset_counter_survives_reopen ( const std::filesystem::path &directory )
{
	const auto path = fixture( directory );
	auto source = priml::WsiSession{};
	require( source.open( path ), "source open failed" );
	auto saved = priml::PtfSubsetResult{};
	auto error = std::string{};
	const auto view = priml::Rtlbr<double>{ { 0, 0 }, { 8, 8 } };
	require( priml::save_ptf_subset( source, view, &saved, &error ), error );
	require( saved.path.filename() == "sub_1_source.ptf", "first filename wrong" );
	const auto first_size = std::filesystem::file_size( saved.path );
	source.close();
	require( source.open( path ), "source reopen failed" );
	std::ofstream( directory / "sub_7_source.ptf" ) << "existing sentinel";
	require( priml::save_ptf_subset( source, view, &saved, &error ), error );
	require( saved.path.filename() == "sub_8_source.ptf", "counter did not continue past existing files" );
	require( std::filesystem::file_size( directory / "sub_1_source.ptf" ) == first_size &&
		std::filesystem::file_size( directory / "sub_7_source.ptf" ) == 17, "an existing file was replaced" );
}


void test_ptf_subset_refuses_irregular_tiles ( const std::filesystem::path &directory )
{
	auto source = priml::WsiSession{};
	require( source.open( fixture( directory ) ), "source open failed" );
	auto &tile = source.pyramid().levels.front().tiles[1];
	--tile.lr.tl.x;
	--tile.lr.br.x;
	auto saved = priml::PtfSubsetResult{};
	auto error = std::string{};
	require( !priml::save_ptf_subset( source, { { 0, 0 }, { 48, 32 } }, &saved, &error ) &&
		error.find( "irregular" ) != std::string::npos, "irregular layout was accepted" );
	require( std::distance( std::filesystem::directory_iterator( directory ),
		std::filesystem::directory_iterator{} ) == 1, "failed export left artifacts" );
}


void test_ptf_subset_empty_view_creates_no_file ( const std::filesystem::path &directory )
{
	auto source = priml::WsiSession{};
	require( source.open( fixture( directory ) ), "source open failed" );
	auto saved = priml::PtfSubsetResult{};
	auto error = std::string{};
	require( !priml::save_ptf_subset( source, { { 1000, 1000 }, { 1001, 1001 } }, &saved, &error ),
		"out-of-bounds viewport accepted" );
	require( !error.empty() && saved.path.empty(), "empty export did not report an error" );
}


void test_ptf_subset_shared_jpeg_tables_and_tiff_reader ( const std::filesystem::path &directory )
{
	std::filesystem::create_directories( directory );
	const auto path = directory / "source.svs";
	auto *tif = TIFFOpen( path.string().c_str(), "w8" );
	require( tif != nullptr, "cannot create SVS fixture" );
	auto file = std::unique_ptr<TIFF, decltype( &TIFFClose )>( tif, TIFFClose );
	TIFFSetField( tif, TIFFTAG_IMAGEWIDTH, (uint32_t)64 );
	TIFFSetField( tif, TIFFTAG_IMAGELENGTH, (uint32_t)32 );
	TIFFSetField( tif, TIFFTAG_TILEWIDTH, (uint32_t)32 );
	TIFFSetField( tif, TIFFTAG_TILELENGTH, (uint32_t)32 );
	TIFFSetField( tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)3 );
	TIFFSetField( tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)8 );
	TIFFSetField( tif, TIFFTAG_PLANARCONFIG, (uint16_t)PLANARCONFIG_CONTIG );
	TIFFSetField( tif, TIFFTAG_PHOTOMETRIC, (uint16_t)PHOTOMETRIC_YCBCR );
	TIFFSetField( tif, TIFFTAG_COMPRESSION, (uint16_t)COMPRESSION_JPEG );
	TIFFSetField( tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB );
	TIFFSetField( tif, TIFFTAG_JPEGQUALITY, 95 );
	TIFFSetField( tif, TIFFTAG_JPEGTABLESMODE, JPEGTABLESMODE_QUANT | JPEGTABLESMODE_HUFF );
	TIFFSetField( tif, TIFFTAG_RESOLUTIONUNIT, (uint16_t)RESUNIT_CENTIMETER );
	TIFFSetField( tif, TIFFTAG_XRESOLUTION, 20000.0 );
	TIFFSetField( tif, TIFFTAG_YRESOLUTION, 20000.0 );
	TIFFSetField( tif, TIFFTAG_IMAGEDESCRIPTION,
		"Aperio Image Library v12.0.0\n64x32 [0,0 64x32] (32x32) JPEG Q=95|MPP = 0.5" );
	auto pixels = std::vector<uint8_t>( 32 * 32 * 3 );
	for (auto i = 0; i < 32 * 32; ++i) {
		pixels[i * 3] = 210;
		pixels[i * 3 + 1] = 40;
		pixels[i * 3 + 2] = 80;
	}
	for (auto tile = 0; tile < 2; ++tile)
		require( TIFFWriteEncodedTile( tif, tile, pixels.data(), (tmsize_t)pixels.size() ) > 0,
			"cannot encode shared-table JPEG fixture" );
	require( TIFFWriteDirectory( tif ) != 0, "cannot finish JPEG fixture" );
	file.reset();
	auto source = priml::WsiSession{};
	require( source.open( path ), "SVS source open failed" );
	require( !source.pyramid().levels.front().hdrbf.empty(), "fixture did not store shared JPEG tables" );
	auto saved = priml::PtfSubsetResult{};
	auto error = std::string{};
	require( priml::save_ptf_subset( source, { { 17, 1 }, { 18, 2 } }, &saved, &error ), error );
	compare_base( source, saved );
	auto handle = priml::TiffHandle{};
	require( handle.open( saved.path.string() ), "TIFF cannot open export" );
	auto rgba = std::vector<uint32_t>( 32 * 32 );
	require( TIFFReadRGBAImageOriented( (TIFF *)handle.get(), 32, 32, rgba.data(), ORIENTATION_TOPLEFT, 1 ) != 0,
		"native libtiff cannot decode exported JPEG tiles" );
	require( std::abs( (int)TIFFGetR( rgba[0] ) - 210 ) < 5 &&
		std::abs( (int)TIFFGetG( rgba[0] ) - 40 ) < 5 &&
		std::abs( (int)TIFFGetB( rgba[0] ) - 80 ) < 5, "TIFF colour interpretation changed" );
}


void test_ptf_subset_reduced_levels_keep_base_bytes ( const std::filesystem::path &directory )
{
	auto source = priml::WsiSession{};
	require( source.open( fixture( directory, { 1059, 35 } ) ), "source open failed" );
	auto saved = priml::PtfSubsetResult{};
	auto error = std::string{};
	require( priml::save_ptf_subset( source, { { 0, 0 }, { 530, 18 } }, &saved, &error ), error );
	compare_base( source, saved );
	auto subset = priml::WsiSession{};
	require( subset.open( saved.path ), "subset open failed" );
	const auto &stack = subset.pyramid().planes.front();
	require( stack.ladder.size() == 3 && saved.stats.levels_written == 3, "reduced pyramid missing" );
	require( subset.pyramid().levels[stack.ladder[1]].metric == 1.0f &&
		subset.pyramid().levels[stack.ladder[2]].metric == 2.0f, "odd dimensions distorted calibration" );
	const auto dims = priml::Dim2D<int>{ 265, 9 };
	auto image = subset.loader()->Generate<uint8_t>( subset.pyramid(), nullptr, 2,
		{ { 0, 0 }, { 530, 18 } }, &dims );
	require( image.p && image.w == 265 && image.h == 9 && image.rowp( 2 )[2] > 0,
		"reduced level cannot render" );
}


void test_ptf_subset_failed_reduction_cleans_staging ( const std::filesystem::path &directory )
{
	auto source = priml::WsiSession{};
	require( source.open( fixture( directory, { 1059, 35 } ) ), "source open failed" );
	auto saved = priml::PtfSubsetResult{};
	auto error = std::string{};
	auto options = priml::PtfSubsetOptions{};
	options.reduced_codec = priml::ImgCodec::None;
	require( !priml::save_ptf_subset( source, { { 0, 0 }, { 530, 18 } }, &saved, &error, options ),
		"failed reduction was published" );
	require( std::distance( std::filesystem::directory_iterator( directory ),
		std::filesystem::directory_iterator{} ) == 1 && saved.path.empty(), "failed reduction left artifacts" );
}


void test_ptf_subset_sparse_tiles_remain_empty ( const std::filesystem::path &directory )
{
	auto source = priml::WsiSession{};
	require( source.open( fixture( directory, { 96, 64 }, false, true ) ), "source open failed" );
	auto saved = priml::PtfSubsetResult{};
	auto error = std::string{};
	require( priml::save_ptf_subset( source, { { 0, 0 }, { 48, 32 } }, &saved, &error ), error );
	compare_base( source, saved );
	auto subset = priml::WsiSession{};
	require( subset.open( saved.path ), "subset open failed" );
	const auto size = priml::Dim2D<int>{ 32, 32 };
	auto gap = subset.loader()->Generate<uint8_t>( subset.pyramid(), nullptr, 0.5,
		{ { 16, 0 }, { 32, 16 } }, &size );
	require( gap.p && std::all_of( gap.buf.begin(), gap.buf.end(), [] ( auto pixel ) { return pixel == 0; } ),
		"sparse tile gap was filled with unrelated pixels" );
}


void test_ptf_subset_read_failure_cleans_staging ( const std::filesystem::path &directory )
{
	auto source = priml::WsiSession{};
	const auto path = fixture( directory );
	require( source.open( path ), "source open failed" );
	source.pyramid().levels.front().tiles[1].foff = std::filesystem::file_size( path ) + 4096;
	auto saved = priml::PtfSubsetResult{};
	auto error = std::string{};
	require( !priml::save_ptf_subset( source, { { 0, 0 }, { 48, 32 } }, &saved, &error ),
		"unreadable base tile was silently skipped" );
	require( !error.empty() && saved.path.empty() &&
		std::distance( std::filesystem::directory_iterator( directory ),
			std::filesystem::directory_iterator{} ) == 1, "read failure left an incomplete export" );
}


// The reduced-level generator on its own, as a caller other than subset uses it:
// appended to a file that holds only its base level.
void test_ptf_reduced_levels_appended_directly ( const std::filesystem::path &directory )
{
	const auto path = fixture( directory, { 1059, 35 } );
	auto base = priml::PtfLevelSpec{};
	base.tags.valid = true;
	base.tags.mpp = 0.5;
	base.tags.pixel_type = priml::LevelPixelType::Gray8;
	base.tags.max_pixel_value = 255;
	base.tags.focus_merge = "method=test";
	base.size = { 1059, 35 };
	auto stats = priml::PtfWriteStats{};
	auto error = std::string{};
	require( priml::append_ptf_reduced_levels( path, { base }, {}, &stats, &error ), error );
	require( stats.levels_written == 2, "two reductions expected for 1059 px" );
	auto reopened = priml::WsiSession{};
	require( reopened.open( path ), "file with appended levels does not open" );
	const auto &stack = reopened.pyramid().planes.front();
	require( stack.ladder.size() == 3 &&
		reopened.pyramid().levels[stack.ladder[1]].metric == 1.0f &&
		reopened.pyramid().levels[stack.ladder[2]].metric == 2.0f, "appended ladder wrong" );
	require( !priml::append_ptf_reduced_levels( path, {}, {}, &stats, &error ) &&
		error.find( "no base level" ) != std::string::npos, "an empty base was accepted" );
}


void test_ptf_tags_focus_merge_round_trip ( const std::filesystem::path & )
{
	auto tags = priml::PtfPlaneTags{};
	tags.valid = true;
	tags.focus_merge = "method=windowed-pick planes=5\nreference=2";
	const auto parsed = priml::parse_ptf_tags( priml::format_ptf_tags( tags ) );
	require( parsed.valid && parsed.focus_merge == "method=windowed-pick planes=5reference=2",
		"focus-merge evidence did not round-trip on one line" );
	require( priml::parse_ptf_tags( priml::format_ptf_tags( priml::PtfPlaneTags{ .valid = true } ) )
		.focus_merge.empty(), "an absent focus merge came back" );
}


#if USE_OPENJPEG
void test_ptf_subset_deep_channels ( const std::filesystem::path &directory )
{
	auto source = priml::WsiSession{};
	require( source.open( fixture( directory, { 1059, 35 }, true ) ), "source open failed" );
	auto saved = priml::PtfSubsetResult{};
	auto error = std::string{};
	auto options = priml::PtfSubsetOptions{};
	options.reduced_codec = priml::ImgCodec::Jp2k;
	options.reduced_encode.quality = priml::kImageLosslessQuality;
	require( priml::save_ptf_subset( source, { { 0, 0 }, { 530, 18 } }, &saved, &error, options ), error );
	compare_base( source, saved );
	require( saved.stats.levels_written == 6, "not every deep channel has its reduced ladder" );
}
#endif

} // namespace


bool run_ptf_subset_tests ( const std::filesystem::path &output )
{
	const auto run = output / ( "ptf_subset_" + std::to_string(
		std::chrono::steady_clock::now().time_since_epoch().count() ) );
	auto passed = true;
	const auto test = [&] ( const char *name, const auto &function ) {
		const auto started = std::chrono::steady_clock::now();
		try {
			function( run / name );
			std::cout << name << ": PASS";
		} catch (const std::exception &exception) {
			passed = false;
			std::cout << name << ": FAIL: " << exception.what();
		}
		std::cout << " (" << std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - started ).count() << " us)\n";
	};
	test( "test_ptf_subset_complete_intersecting_tiles", test_ptf_subset_complete_intersecting_tiles );
	test( "test_level_tile_codec_recorded_by_render", test_level_tile_codec_recorded_by_render );
	test( "test_ptf_subset_half_open_boundary", test_ptf_subset_half_open_boundary );
	test( "test_ptf_subset_partial_source_edge", test_ptf_subset_partial_source_edge );
	test( "test_ptf_subset_counter_survives_reopen", test_ptf_subset_counter_survives_reopen );
	test( "test_ptf_subset_refuses_irregular_tiles", test_ptf_subset_refuses_irregular_tiles );
	test( "test_ptf_subset_empty_view_creates_no_file", test_ptf_subset_empty_view_creates_no_file );
	test( "test_ptf_subset_shared_jpeg_tables_and_tiff_reader", test_ptf_subset_shared_jpeg_tables_and_tiff_reader );
	test( "test_ptf_subset_reduced_levels_keep_base_bytes", test_ptf_subset_reduced_levels_keep_base_bytes );
	test( "test_ptf_subset_failed_reduction_cleans_staging", test_ptf_subset_failed_reduction_cleans_staging );
	test( "test_ptf_subset_sparse_tiles_remain_empty", test_ptf_subset_sparse_tiles_remain_empty );
	test( "test_ptf_subset_read_failure_cleans_staging", test_ptf_subset_read_failure_cleans_staging );
	test( "test_ptf_reduced_levels_appended_directly", test_ptf_reduced_levels_appended_directly );
	test( "test_ptf_tags_focus_merge_round_trip", test_ptf_tags_focus_merge_round_trip );
#if USE_OPENJPEG
	test( "test_ptf_subset_deep_channels", test_ptf_subset_deep_channels );
#endif
	return passed;
}
