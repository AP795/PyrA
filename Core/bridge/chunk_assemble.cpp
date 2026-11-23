// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "chunk_internal.h"
#include "chunk_pyramid.h"

#include "dcm_identity.h"
#include "dcm_series.h"
#include "pyr_rt.h"
#include "wsi.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>

namespace priml::bridge_detail {
namespace {

using Clock = std::chrono::steady_clock;

// Optical Path Identifier is SH: at most 16 characters, suffix included.
inline constexpr size_t kOpticalIdMax = 16;
inline constexpr size_t kOpticalIdStem = 12;


bool assemble_fail ( std::string *error, std::string message )
{
	if (error)
		*error = std::move( message );
	pyr::plog( "pyra process: %s\n", error ? error->c_str() : "" );
	return false;
}


double seconds_since ( Clock::time_point start )
{
	return std::chrono::duration<double>( Clock::now() - start ).count();
}


std::string path_utf8 ( const std::filesystem::path &path )
{
	const auto text = path.u8string();
	return std::string( text.begin(), text.end() );
}


// An identifier of letters, digits, '_' and '-' derived from the plane name,
// distinct from every identifier the series (or this run) already uses.
std::string make_optical_id ( std::string_view name, std::vector<std::string> *used )
{
	auto stem = std::string{};
	for (auto c : name)
		if (stem.size() < kOpticalIdStem)
			stem.push_back( std::isalnum( (unsigned char)c ) || c == '-' ? c : '_' );
	if (stem.empty())
		stem = "DERIVED";
	auto id = stem;
	for (auto n = 2; std::find( used->begin(), used->end(), id ) != used->end(); ++n)
		id = std::format( "{}_{}", stem, n ).substr( 0, kOpticalIdMax );
	used->push_back( id );
	return id;
}


// One rectangle per run of present frames in each frame row of a level.
template <typename Present>
std::vector<Rtlbr<int>> coverage_runs ( Dim2D<int> size, Dim2D<int> tile, Present &&present )
{
	const auto across = 1 + ( size.w - 1 ) / tile.w;
	const auto down = 1 + ( size.h - 1 ) / tile.h;
	auto runs = std::vector<Rtlbr<int>>{};
	for (auto ty = 0; ty < down; ++ty)
		for (auto tx = 0; tx < across; ) {
			if (!present( tx, ty )) {
				++tx;
				continue;
			}
			const auto first = tx;
			while (tx < across && present( tx, ty ))
				++tx;
			runs.push_back( { { first * tile.w, ty * tile.h },
				{ std::min( size.w, tx * tile.w ), std::min( size.h, ( ty + 1 ) * tile.h ) } } );
		}
	return runs;
}


// What a derived instance says about how it was made. Starts with the marker
// remove_dcm_channel looks for, and stays one line.
std::string evidence ( const AssemblyInput &input, std::string_view raster )
{
	const auto &options = *input.options;
	auto text = std::format( "{} script={} sha256={}", kDcmDerivedChannelMarker,
		input.process_script, input.process_sha256 );
	if (input.prepared)
		text += std::format( " prepare={} sha256={} prepare-mpp={:.6g}", input.prepared->script,
			input.prepared->sha256, input.prepared->mpp );
	text += std::format( " source={} z={} chunk={}x{} halo={} reduce=mean2x2",
		options.source.channel.empty() ? "(default)" : options.source.channel, options.source.zlevel,
		input.chunk.w, input.chunk.h, options.halo );
	if (!raster.empty())
		text += std::format( " raster={}", raster );
	auto params = std::string{};
	for (const auto &[key, value] : input.params)
		params += ( params.empty() ? "" : ";" ) + key + "=" + value;
	if (!params.empty())
		text += " params=" + params;
	return text;
}


DcmLevelSpec base_spec ( const AssemblyInput &input, const std::string &channel, const std::string &optical_id,
	LevelPixelType type, std::string processing, const Pxy<double> &origin_um )
{
	const auto &source = *input.source;
	auto spec = DcmLevelSpec{};
	spec.channel = channel;
	spec.optical_path_id = optical_id;
	spec.processing = std::move( processing );
	spec.level = 0;
	spec.pixel_type = type;
	spec.max_pixel_value = type == LevelPixelType::Gray16 ? 65535 : 255;
	spec.mpp = source.mpp;
	spec.origin_um = origin_um;
	spec.size = source.size;
	spec.tile = source.tile;
	spec.codec = input.options->codec;
	spec.encode.quality = kImageLosslessQuality;
	spec.sparse = true;
	return spec;
}


// The levels coarser than a chunk: halving from `from` until a level fits one
// frame, each rendered from the instances written so far. Every instance repeats
// the base evidence: removal identifies a derived channel by it, instance by
// instance. from_instance is taken by value: the caller's is usually an element
// of the writer's instance list, which every level written here may reallocate.
template <typename P>
bool write_reduced_levels ( DcmWriter &writer, const std::filesystem::path from_instance,
	const DcmLevelSpec &from, std::string *error )
{
	auto size = from.size;
	auto mpp = from.mpp;
	const auto plane = PlaneSel{ from.channel, 0.0f };
	for (auto level = from.level + 1; size.w > from.tile.w || size.h > from.tile.h; ++level) {
		size = { 1 + ( size.w - 1 ) / 2, 1 + ( size.h - 1 ) / 2 };
		mpp *= 2.0;
		auto pyramid = Ppyr{};
		pyramid.coarse_level_bytes = 0;
		pyramid.use_pixbf = false;
		auto loader = WsiLoader::Create( path_utf8( from_instance ) );
		if (!loader || !loader->OpenFile<P>( path_utf8( from_instance ), pyramid ))
			return assemble_fail( error, std::format( "cannot reopen the written levels of '{}' for level {}{}",
				from.channel, level, loader && !loader->open_error_.empty() ? ": " + loader->open_error_ : std::string{} ) );
		const auto geometry = native_geometry_for_plane( pyramid, plane );
		if (!geometry)
			return assemble_fail( error, "the written levels of '" + from.channel + "' are not one plane" );
		const auto origin = geometry->metric_bounds.tl;
		auto spec = from;
		spec.level = level;
		spec.size = size;
		spec.mpp = mpp;
		spec.coverage.clear();
		spec.sparse = false;
		auto render_failed = false;
		const auto fetch = DcmTileFetch<P>( [&] ( const Rtlbr<int> &rect, Pwhl<P> *out, std::string * ) {
			const auto metric = Rtlbr<float>{
				{ (float)( origin.x + rect.tl.x * mpp ), (float)( origin.y + rect.tl.y * mpp ) },
				{ (float)( origin.x + rect.br.x * mpp ), (float)( origin.y + rect.br.y * mpp ) } };
			const auto dims = Dim2D<int>{ width( rect ), height( rect ) };
			const auto image = loader->Generate<P>( pyramid, nullptr, (float)mpp, metric, &dims, plane );
			if (!image.p || image.w != dims.w || image.h != dims.h) {
				render_failed = true;
				return DcmFetchResult::Error;
			}
			for (auto y = 0; y < dims.h; ++y)
				std::memcpy( out->rowp( y ), image.rowp( y ), (size_t)dims.w * sizeof( P ) );
			return DcmFetchResult::Pixels;
		} );
		const auto written = writer.add_level<P>( spec, fetch, error );
		loader->Close( pyramid );
		if (render_failed)
			return assemble_fail( error, "cannot render reduced level " + std::to_string( level ) + " of '" + from.channel + "'" );
		if (!written)
			return false;
	}
	return true;
}


// One plane: levels 0..chunk_levels copied from the spool as the workers encoded
// them, never decoded, then the coarser levels rendered from the last of those.
// False with an empty error when the plane has no frame at all.
bool write_plane ( const AssemblyInput &input, DcmWriter &writer, std::ifstream &spool, int plane,
	const DcmLevelSpec &base, std::string *error )
{
	const auto &source = *input.source;
	const auto &index = *input.index;
	auto spec = base;
	auto last_instance = std::filesystem::path{};
	for (auto level = 0; level <= input.chunk_levels; ++level) {
		spec.level = level;
		spec.size = { level_extent( source.size.w, level ), level_extent( source.size.h, level ) };
		spec.mpp = base.mpp * (double)( 1 << level );
		const auto across = 1 + ( spec.size.w - 1 ) / spec.tile.w;
		spec.coverage = coverage_runs( spec.size, spec.tile, [&] ( int tx, int ty ) {
			return index.contains( spool_key( plane, level, (int64_t)ty * across + tx ) );
		} );
		if (spec.coverage.empty()) {
			if (level == 0) {
				error->clear();
				return false;
			}
			return assemble_fail( error, std::format( "plane '{}' has no frame at level {}", spec.channel, level ) );
		}
		const auto fetch = DcmEncodedFetch( [&] ( const Rtlbr<int> &rect, std::vector<uint8_t> *bytes, std::string *fetch_error ) {
			const auto key = spool_key( plane, level, (int64_t)( rect.tl.y / spec.tile.h ) * across + rect.tl.x / spec.tile.w );
			const auto found = index.find( key );
			if (found == index.end())
				return DcmFetchResult::Absent;
			bytes->resize( found->second.length );
			spool.seekg( (std::streamoff)found->second.offset );
			spool.read( (char *)bytes->data(), (std::streamsize)bytes->size() );
			if (!spool) {
				assemble_fail( fetch_error, "cannot read the frame spool" );
				return DcmFetchResult::Error;
			}
			return DcmFetchResult::Pixels;
		} );
		if (!writer.add_encoded_level( spec, fetch, error ))
			return false;
		last_instance = writer.stats().instances.back();
	}
	return spec.pixel_type == LevelPixelType::Gray16
		? write_reduced_levels<uint16_t>( writer, last_instance, spec, error )
		: write_reduced_levels<uint8_t>( writer, last_instance, spec, error );
}

} // namespace


bool assemble_dcm_channels ( const AssemblyInput &input, ChunkProcessStats *stats, std::string *error )
{
	auto local_error = std::string{};
	if (!error)
		error = &local_error;
	const auto &options = *input.options;
	auto meta = DcmAcquisition{};
	auto origin_um = Pxy<double>{};
	if (!dcm_slide_acquisition( input.slide, &meta, &origin_um, error ))
		return false;
	auto used = input.optical_ids_in_use;
	const auto optical_id = make_optical_id( options.channel, &used );
	const auto &identity = dcm_writer_identity();
	meta.uid_root = options.uid_root.empty() ? default_dcm_uid_root() : options.uid_root;
	meta.name = optical_id;
	meta.manufacturer = identity.manufacturer;
	meta.model = identity.model;
	meta.serial = "1";
	meta.software = "pyra_process";
	meta.derivation_description = "Python processing: " + input.process_script;
	const auto reduce_started = Clock::now();
	auto spool = std::ifstream( input.spool, std::ios::binary );
	if (!spool)
		return assemble_fail( error, "cannot reopen the frame spool" );
	auto writer = DcmWriter{};
	if (!writer.open( input.staging / "instances", meta, error ))
		return false;
	auto base = base_spec( input, options.channel, optical_id, options.output_type,
		evidence( input, {} ), origin_um );
//
// State the largest value the channel holds, not the container's: viewers size
// their display range by it, and a density scaled into a few thousand counts
// would otherwise show as black in a 0..65535 range. Reduced levels are means,
// so they never exceed it.
	base.max_pixel_value = std::clamp( (int)input.result_max, 1, base.max_pixel_value );
	if (!write_plane( input, writer, spool, 0, base, error ))
		return error->empty() ? assemble_fail( error, "the processed channel has no frame" ) : false;
	stats->channels.push_back( options.channel );
	if (input.prepared)
		for (auto i = size_t{}; i < input.prepared->stored.size(); ++i) {
			const auto &name = input.prepared->stored[i];
			const auto channel = options.channel + "." + name;
			const auto spec = base_spec( input, channel, make_optical_id( channel, &used ),
				LevelPixelType::Gray8, evidence( input, name ), origin_um );
			if (write_plane( input, writer, spool, (int)i + 1, spec, error ))
				stats->channels.push_back( channel );
			else if (!error->empty())
				return false;
			else
				pyr::plog( "pyra process: raster '%s' is empty everywhere; not stored\n", name.c_str() );
		}
	if (!writer.close( error ))
		return false;
	stats->reduce_s = seconds_since( reduce_started );
	auto written = writer.stats();
	stats->frames_written = written.tiles_written;
	stats->encoded_bytes = written.encoded_bytes;
	const auto publish_started = Clock::now();
	if (!publish_dcm_companions( input.slide, &written, error ))
		return false;
	stats->publish_s = seconds_since( publish_started );
	stats->published = written.instances;
	return true;
}

} // namespace priml::bridge_detail
