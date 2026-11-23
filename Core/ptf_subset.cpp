// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "ptf_subset.h"

#include "pyr_rt.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>

namespace priml {
namespace {

struct PlaneSubset {
	int level = -1;
	PtfLevelSpec spec;
	std::vector<int> selected;
	std::map<std::pair<int, int>, int> tiles;
};


bool fail ( std::string *error, const std::string &message )
{
	if (error)
		*error = "Save WSI subset: " + message;
	return false;
}


std::string utf8 ( const std::filesystem::path &path )
{
	const auto text = path.u8string();
	return { text.begin(), text.end() };
}


bool intersects ( const Rtlbr<int> &tile, double mpp, const Rtlbr<double> &view )
{
	return tile.tl.x * mpp < view.br.x && tile.br.x * mpp > view.tl.x &&
		tile.tl.y * mpp < view.br.y && tile.br.y * mpp > view.tl.y;
}


bool plan_subset ( WsiSession &source, const Rtlbr<double> &view,
	std::vector<PlaneSubset> *planes, Rtlbr<int> *bounds, std::string *error )
{
	if (!source.is_open() || view.is_empty() ||
		!std::isfinite( view.tl.x ) || !std::isfinite( view.tl.y ) ||
		!std::isfinite( view.br.x ) || !std::isfinite( view.br.y ))
		return fail( error, "no open WSI or the viewport is empty/invalid" );
	const auto &pyramid = source.pyramid();
	const auto metric = pyramid.source_pixel_metric;
	if (metric.x > 0 && metric.y > 0 && std::abs( metric.x - metric.y ) > metric.x * 1e-5)
		return fail( error, "unequal X/Y pixel spacing cannot be preserved by this PTF writer" );
	auto first = true;
	auto mpp = 0.0f;
	for (const auto &stack : pyramid.planes) {
		if (stack.ladder.empty())
			continue;
		auto plane = PlaneSubset{};
		plane.level = stack.ladder.front();
		const auto &level = pyramid.levels[plane.level];
		if (level.synthetic || !std::isfinite( level.metric ) || level.metric <= 0 ||
			level.tile_w <= 0 || level.tile_h <= 0 || level.tile_w % 16 || level.tile_h % 16)
			return fail( error, "level-0 tile dimensions must be positive multiples of 16 for TIFF; no base re-encoding is allowed" );
		if (mpp > 0 && level.metric != mpp)
			return fail( error, "planes with different native pixel spacing cannot share this lossless crop" );
		mpp = level.metric;
		plane.spec.tags.valid = true;
		plane.spec.tags.channel = stack.plane.channel;
		plane.spec.tags.zlevel = stack.plane.zlevel;
		plane.spec.tags.pixel_type = level.pixel_type;
		plane.spec.tags.max_pixel_value = level.max_pixel_value;
		plane.spec.tags.mpp = mpp;
		plane.spec.tile = { level.tile_w, level.tile_h };
		plane.spec.color_hint = level.color_hint;
		plane.spec.icc_profile = pyramid.icc_profile;
		auto occupied = false;
		for (auto i = 0; i < (int)level.tiles.size(); ++i) {
			const auto &tile = level.tiles[i];
			if (tile.lr.is_empty() || !intersects( tile.lr, mpp, view ))
				continue;
			plane.selected.push_back( i );
			occupied = occupied || tile.fsz > 0;
		}
		if (!occupied)
			continue;
		for (auto i : plane.selected) {
			const auto &rect = level.tiles[i].lr;
			if (first) {
				*bounds = rect;
				first = false;
			} else {
				bounds->tl.x = std::min( bounds->tl.x, rect.tl.x );
				bounds->tl.y = std::min( bounds->tl.y, rect.tl.y );
				bounds->br.x = std::max( bounds->br.x, rect.br.x );
				bounds->br.y = std::max( bounds->br.y, rect.br.y );
			}
		}
		planes->push_back( std::move( plane ) );
	}
	if (planes->empty())
		return fail( error, "the viewport intersects no stored level-0 pixels" );
	if ((int64_t)bounds->br.x - bounds->tl.x > INT_MAX ||
		(int64_t)bounds->br.y - bounds->tl.y > INT_MAX)
		return fail( error, "subset dimensions exceed the supported TIFF pixel range" );
	for (auto &plane : *planes) {
		plane.spec.size = { width( *bounds ), height( *bounds ) };
		const auto &level = pyramid.levels[plane.level];
		for (auto i : plane.selected) {
			const auto &rect = level.tiles[i].lr;
			const auto x = rect.tl.x - bounds->tl.x;
			const auto y = rect.tl.y - bounds->tl.y;
			if (x % level.tile_w || y % level.tile_h ||
				width( rect ) != std::min( level.tile_w, bounds->br.x - rect.tl.x ) ||
				height( rect ) != std::min( level.tile_h, bounds->br.y - rect.tl.y ) ||
				!plane.tiles.emplace( std::pair{ x, y }, i ).second)
				return fail( error, "irregular or overlapping level-0 tiles cannot fit a TIFF grid unchanged" );
		}
		auto bytes = std::vector<uint8_t>{};
		const auto sample = std::find_if( plane.selected.begin(), plane.selected.end(),
			[&] ( int i ) { return level.tiles[i].fsz > 0; } );
		if (!source.loader()->ReadEncodedTile( pyramid, plane.level, *sample, &bytes, error ))
			return false;
		plane.spec.codec = sniff_img_codec( bytes.data(), bytes.size() );
		if (plane.spec.codec != ImgCodec::Jpeg && plane.spec.codec != ImgCodec::Jp2k &&
			plane.spec.codec != ImgCodec::Htj2k && plane.spec.codec != ImgCodec::Jxl)
			return fail( error, "source tile encoding cannot be stored unchanged in PTF" );
	}
	return true;
}


void add_stats ( PtfWriteStats *total, const PtfWriteStats &stats )
{
	total->levels_written += stats.levels_written;
	total->tiles_written += stats.tiles_written;
	total->tiles_empty += stats.tiles_empty;
	total->encoded_bytes += stats.encoded_bytes;
}


bool write_base ( WsiSession &source, const std::filesystem::path &stage,
	const std::vector<PlaneSubset> &planes, PtfSubsetResult *result,
	std::string *error, const PtfSubsetProgress &progress )
{
	auto writer = PtfWriter{};
	if (!writer.open( utf8( stage ), error ))
		return false;
	auto completed = size_t{};
	auto total = size_t{};
	for (const auto &plane : planes)
		total += plane.selected.size();
	for (const auto &plane : planes) {
		auto fetch = [&] ( const Rtlbr<int> &rect, std::vector<uint8_t> *bytes,
			std::string *fetch_error ) {
			const auto found = plane.tiles.find( { rect.tl.x, rect.tl.y } );
			if (found == plane.tiles.end())
				return PtfEncodedFetchResult::Absent;
			if (progress)
				progress( ++completed, total );
			if (!source.loader()->ReadEncodedTile( source.pyramid(), plane.level,
				found->second, bytes, fetch_error ))
				return PtfEncodedFetchResult::Error;
			return bytes->empty() ? PtfEncodedFetchResult::Absent : PtfEncodedFetchResult::Tile;
		};
		if (!writer.add_encoded_level( plane.spec, fetch, error ))
			return false;
	}
	if (!writer.close( error ))
		return false;
	add_stats( &result->stats, writer.stats() );
	return true;
}


bool write_reduced ( const std::filesystem::path &stage,
	const std::vector<PlaneSubset> &planes, const PtfSubsetOptions &options,
	PtfSubsetResult *result, std::string *error, const PtfSubsetProgress &progress )
{
	auto base = std::vector<PtfLevelSpec>{};
	for (const auto &plane : planes)
		base.push_back( plane.spec );
	return append_ptf_reduced_levels( stage, base, options, &result->stats, error, progress );
}


class StagingDirectory {
public:
	std::filesystem::path path;
	~StagingDirectory () {
		if (!path.empty()) {
			auto ignored = std::error_code{};
			std::filesystem::remove_all( path, ignored );
		}
	}
};


uint64_t next_number ( const std::filesystem::path &directory,
	const std::string &stem )
{
	auto highest = uint64_t{};
	const auto suffix = "_" + stem + ".ptf";
	for (const auto &entry : std::filesystem::directory_iterator( directory )) {
		const auto name = utf8( entry.path().filename() );
		if (!name.starts_with( "sub_" ) || !name.ends_with( suffix ) ||
			name.size() <= 4 + suffix.size())
			continue;
		const auto digits = std::string_view( name ).substr( 4, name.size() - 4 - suffix.size() );
		auto number = uint64_t{};
		const auto parsed = std::from_chars( digits.data(), digits.data() + digits.size(), number );
		if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size())
			highest = std::max( highest, number );
	}
	if (highest == std::numeric_limits<uint64_t>::max())
		throw std::runtime_error( "subset filename counter is exhausted" );
	return highest + 1;
}

} // namespace


bool append_ptf_reduced_levels ( const std::filesystem::path &path,
	const std::vector<PtfLevelSpec> &base, const PtfSubsetOptions &options,
	PtfWriteStats *stats, std::string *error, const PtfSubsetProgress &progress )
{
	const auto failed = [error] ( const std::string &message ) {
		if (error)
			*error = "PTF reduced levels: " + message;
		return false;
	};
	if (base.empty() || !base.front().size.is_set() || !( base.front().tags.mpp > 0.0 ))
		return failed( "no base level to reduce" );
	auto size = base.front().size;
	auto mpp = base.front().tags.mpp;
	while (size.w > kPtfDefaultTileEdge || size.h > kPtfDefaultTileEdge) {
		size = { 1 + ( size.w - 1 ) / 2, 1 + ( size.h - 1 ) / 2 };
		mpp *= 2;
// Reopen after each reduction: the next step renders at most one preceding
// level's 2x2 tile neighbourhood, even for a whole-slide viewport.
		auto pyramid = Ppyr{};
		pyramid.coarse_level_bytes = 0;
		pyramid.use_pixbf = false;
		auto loader = WsiLoader::Create( utf8( path ) );
		const auto opened = visit_level_pixel_type( base.front().tags.pixel_type,
			[&] ( auto pixel ) { return loader && loader->OpenFile<decltype( pixel )>( utf8( path ), pyramid ); } );
		if (!opened)
			return failed( "cannot reopen the written levels to generate reduced levels" );
		auto writer = PtfWriter{};
		if (!writer.open_append( utf8( path ), error ))
			return false;
		auto completed = size_t{};
		const auto total = (size_t)( 1 + ( size.w - 1 ) / kPtfDefaultTileEdge ) *
			( 1 + ( size.h - 1 ) / kPtfDefaultTileEdge ) * base.size();
		for (const auto &level : base) {
			auto spec = level;
			spec.size = size;
			spec.tile = { kPtfDefaultTileEdge, kPtfDefaultTileEdge };
			spec.tags.mpp = mpp;
//
// Evidence of what was done to the base pixels describes the base level only.
			spec.tags.registration.clear();
			spec.tags.flat_field.clear();
			spec.tags.focus_merge.clear();
			spec.codec = options.reduced_codec;
			spec.encode = options.reduced_encode;
			spec.color_hint = ColorHint::FromStream;
			spec.icc_profile.clear();
			auto render_failed = false;
			const auto written = visit_level_pixel_type( spec.tags.pixel_type, [&] ( auto pixel ) {
				using Pixel = decltype( pixel );
				auto fetch = PtfTileFetch<Pixel>( [&] ( const Rtlbr<int> &rect, Pwhl<Pixel> *out ) {
					const auto metric_rect = Rtlbr<float>{
						{ (float)( rect.tl.x * mpp ), (float)( rect.tl.y * mpp ) },
						{ (float)( rect.br.x * mpp ), (float)( rect.br.y * mpp ) } };
					const auto dims = Dim2D<int>{ width( rect ), height( rect ) };
					*out = loader->Generate<Pixel>( pyramid, nullptr, (float)mpp,
						metric_rect, &dims, { spec.tags.channel, spec.tags.zlevel } );
					render_failed = render_failed || !out->p || out->w != dims.w || out->h != dims.h;
					if (progress)
						progress( ++completed, total );
					return !render_failed;
				} );
				return writer.add_level( spec, fetch, error );
			} );
			if (render_failed)
				return failed( "cannot decode the written levels to generate the reduced pyramid" );
			if (!written)
				return false;
		}
		if (!writer.close( error ))
			return false;
		if (stats)
			add_stats( stats, writer.stats() );
		loader->Close( pyramid );
	}
	return true;
}


bool save_ptf_subset ( WsiSession &source, const Rtlbr<double> &metric_rect,
	PtfSubsetResult *result, std::string *error, const PtfSubsetOptions &options,
	const PtfSubsetProgress &progress )
{
	if (error)
		error->clear();
	if (result)
		*result = {};
	if (!result)
		return fail( error, "result is null" );
	try {
		auto planes = std::vector<PlaneSubset>{};
		auto bounds = Rtlbr<int>{};
		if (!plan_subset( source, metric_rect, &planes, &bounds, error ))
			return false;
		const auto source_path = std::filesystem::absolute( source.source_path() );
		const auto directory = source_path.parent_path();
		const auto stem = utf8( source_path.stem() );
		auto number = next_number( directory, stem );
		auto staging = StagingDirectory{};
		static auto sequence = std::atomic<uint64_t>{};
		for (;;) {
			const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
			auto candidate = directory / pyr::path_from_utf8(
				std::format( ".ptf-subset-{}-{}", tick, sequence.fetch_add( 1 ) ) );
			if (std::filesystem::create_directory( candidate )) {
				staging.path = std::move( candidate );
				break;
			}
		}
		const auto stage = staging.path / "subset.ptf";
		auto saved = PtfSubsetResult{};
		if (!write_base( source, stage, planes, &saved, error, progress ) ||
			!write_reduced( stage, planes, options, &saved, error, progress ))
			return false;
		for (;;) {
			const auto target = directory / pyr::path_from_utf8(
				std::format( "sub_{}_{}.ptf", number, stem ) );
// Linking the completed staged file publishes atomically without overwriting
// a concurrent export, on both Windows and POSIX. Staging is on the same volume.
			auto ec = std::error_code{};
			std::filesystem::create_hard_link( stage, target, ec );
			if (!ec) {
				saved.path = target;
				break;
			}
			if (ec != std::errc::file_exists)
				return fail( error, "cannot publish completed PTF: " + ec.message() );
			if (number == std::numeric_limits<uint64_t>::max())
				return fail( error, "subset filename counter is exhausted" );
			++number;
		}
		const auto mpp = planes.front().spec.tags.mpp;
		saved.source_rect = { { bounds.tl.x * mpp, bounds.tl.y * mpp },
			{ bounds.br.x * mpp, bounds.br.y * mpp } };
		*result = std::move( saved );
		return true;
	} catch (const std::exception &exception) {
		return fail( error, exception.what() );
	}
}

} // namespace priml
