// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "gallery_scan.h"

#include "img_tiff.h"
#include "ipx_imd.h"
#include "ipx_names.h"
#include "pyr_rt.h"
#include "pyr_text.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

namespace priml {

namespace {

// Run `fn( begin, end )` over [0, count) in contiguous blocks, one per worker, the
// calling thread being worker 0. The scan reads whole files, so a handful of
// workers is what a disk - often a remote one - serves well.
template <typename Fn>
void parallel_for_blocks_local ( int count, Fn &&fn )
{
	constexpr unsigned kMaxScanWorkers = 8;
	const auto hardware = std::max( 1u, std::thread::hardware_concurrency() );
	const auto workers = (int)std::min( { kMaxScanWorkers, hardware, (unsigned)std::max( count, 1 ) } );
	if (workers <= 1) {
		fn( 0, count );
		return;
	}
	auto threads = std::vector<std::thread>{};
	const auto block = ( count + workers - 1 ) / workers;
	for (auto w = 1; w < workers; ++w) {
		const auto begin = std::min( count, w * block );
		const auto end = std::min( count, begin + block );
		if (begin < end)
			threads.emplace_back( [&fn, begin, end] { fn( begin, end ); } );
	}
	fn( 0, std::min( count, block ) );
	for (auto &thread : threads)
		thread.join();
}


std::mutex &ipx_field_namer_mutex_local ()
{
	static auto mutex = std::mutex{};
	return mutex;
}


IpxFieldNamer &ipx_field_namer_storage_local ()
{
	static auto namer = IpxFieldNamer{};
	return namer;
}


IpxFieldNamer ipx_field_namer_local ()
{
	std::scoped_lock lock( ipx_field_namer_mutex_local() );
	return ipx_field_namer_storage_local();
}


// Pixel sizes closer than this (relative) are the same pixel size: the tags and
// .imd records store it as a rounded float.
constexpr double kGalleryMppRelativeTolerance = 1e-4;


bool set_error_local ( std::string *error, const std::string &message )
{
	if (error)
		*error = message;
	return false;
}


std::string utf8_local ( const std::filesystem::path &path )
{
	const auto u8 = path.u8string();
	return std::string( reinterpret_cast<const char *>( u8.data() ), u8.size() );
}


// `path` as a manifest names it: relative to the manifest's directory, '/' separated.
std::string relative_utf8_local ( const std::filesystem::path &path,
	const std::filesystem::path &directory )
{
	const auto u8 = path.lexically_relative( directory ).generic_u8string();
	return std::string( reinterpret_cast<const char *>( u8.data() ), u8.size() );
}


bool is_tiff_extension_local ( const std::filesystem::path &path )
{
	const auto ext = lower_ascii_copy( path.extension().string() );
	return ext == ".tif" || ext == ".tiff";
}


// Natural order: runs of digits compare by value, everything else without case,
// so "cell2" sorts before "cell10".
bool natural_less_local ( const std::string &a, const std::string &b )
{
	auto i = std::size_t{ 0 };
	auto j = std::size_t{ 0 };
	auto is_digit = [] ( char c ) { return c >= '0' && c <= '9'; };
	while (i < a.size() && j < b.size()) {
		if (is_digit( a[i] ) && is_digit( b[j] )) {
			auto ei = i;
			auto ej = j;
			while (ei < a.size() && is_digit( a[ei] ))
				++ei;
			while (ej < b.size() && is_digit( b[ej] ))
				++ej;
			auto si = a.find_first_not_of( '0', i );
			auto sj = b.find_first_not_of( '0', j );
			si = std::min( si == std::string::npos ? ei : si, ei );
			sj = std::min( sj == std::string::npos ? ej : sj, ej );
			const auto li = ei - si;
			const auto lj = ej - sj;
			if (li != lj)
				return li < lj;
			const auto cmp = a.compare( si, li, b, sj, lj );
			if (cmp != 0)
				return cmp < 0;
			i = ei;
			j = ej;
			continue;
		}
		const auto ca = lower_ascii_copy( std::string( 1, a[i] ) )[0];
		const auto cb = lower_ascii_copy( std::string( 1, b[j] ) )[0];
		if (ca != cb)
			return ca < cb;
		++i;
		++j;
	}
	return a.size() - i < b.size() - j;
}


// A file stem as a zone name: characters a zone name may not hold become the slug
// replacement, so the name and its file-system slug stay recognisably the same.
std::string zone_name_from_stem_local ( std::string_view stem )
{
	auto out = std::string{};
	for (auto ch : trim_ascii_copy( stem )) {
		if (ch == kZoneAssocDelimiter || ch == '\t' ||
			static_cast<unsigned char>( ch ) < 0x20 ||
			kZoneNameReservedChars.find( ch ) != std::string_view::npos)
		{
			ch = kZoneSlugReplacementChar;
		}
		out.push_back( ch );
	}
	return out.empty() ? std::string( "zone" ) : out;
}


bool same_mpp_local ( double a, double b )
{
	return std::abs( a - b ) <= kGalleryMppRelativeTolerance * std::max( a, b );
}


// The microns per pixel a member states, and where it said so.
struct StatedMpp {
	double mpp = 0.0;
	std::string source;
};


std::optional<StatedMpp> member_mpp_local ( const TiffImageInfo &info,
	const IpxImdMetadata &imd )
{
	if (imd.loaded && imd.metric_per_pixel > 0.0)
		return StatedMpp{ imd.metric_per_pixel, "imd" };
	if (info.tag_mpp_x > 0.0 && info.tag_mpp_x <= kGalleryMaxPlausibleTagMpp &&
		( !( info.tag_mpp_y > 0.0 ) || same_mpp_local( info.tag_mpp_x, info.tag_mpp_y ) ))
	{
		return StatedMpp{ info.tag_mpp_x, "tiff" };
	}
	return std::nullopt;
}


// The largest sample of `paths`, each read whole and decoded, in parallel because
// the set may sit on a remote disk and this is the one pass that reads every pixel.
// Stops as soon as a sample exceeds `enough`, the caller then knowing all it asked.
// -1 with `error` set when a member cannot be read. When member_maxima is supplied,
// unreadable entries are -1 and the other members are still measured.
//
// `files_read` gets the number of files read so far, counted over every worker but
// called only from the calling thread, which is worker 0 of the pool and reads files
// like the others: progress needs no thread of its own and no barrier between files.
template <typename pixeltype>
int observed_max_local ( const std::vector<std::filesystem::path> &paths, int enough,
	std::string *error, const std::function<void ( std::size_t )> &files_read = {},
	std::vector<int> *member_maxima = nullptr )
{
	if (member_maxima)
		member_maxima->assign( paths.size(), -1 );
	auto max_value = std::atomic<int>{ 0 };
	auto failed = std::atomic<int>{ -1 };
	auto read = std::atomic<std::size_t>{ 0 };
	const auto caller = std::this_thread::get_id();
	parallel_for_blocks_local( (int)paths.size(), [&] ( int begin, int end ) {
		auto bytes = std::vector<uint8_t>{};
		auto buffer = std::vector<pixeltype>{};
		for (auto i = begin; i < end; ++i) {
			if (failed.load() >= 0 || max_value.load() > enough)
				return;
			auto ec = std::error_code{};
			const auto size = std::filesystem::file_size( paths[i], ec );
			auto in = std::ifstream( paths[i], std::ios::binary );
			bytes.resize( ec ? 0 : (size_t)size );
			auto dst = TileDst<pixeltype>{};
			dst.reserve = [&buffer] ( int w, int h, int &stride ) -> pixeltype * {
				buffer.resize( (size_t)w * (size_t)h );
				stride = w;
				return buffer.data();
			};
			if (ec || !in.read( (char *)bytes.data(), (std::streamsize)bytes.size() ) ||
				!tiff_image_decode( bytes.data(), bytes.size(), dst ))
			{
				if (!member_maxima) {
					failed = i;
					return;
				}
				const auto count = ++read;
				if (files_read && std::this_thread::get_id() == caller)
					files_read( count );
				continue;
			}
			auto member_max = 0;
			for (const auto &pixel : buffer) {
				if constexpr (is_rgb_pixel_v<pixeltype>)
					member_max = std::max( { member_max, (int)pixel.r, (int)pixel.g, (int)pixel.b } );
				else
					member_max = std::max( member_max, (int)pixel );
			}
			auto seen = max_value.load();
			if (member_maxima)
				(*member_maxima)[i] = member_max;
			while (member_max > seen && !max_value.compare_exchange_weak( seen, member_max ))
				;
			const auto count = ++read;
			if (files_read && std::this_thread::get_id() == caller)
				files_read( count );
		}
	} );
	if (const auto index = failed.load(); index >= 0) {
		set_error_local( error, "cannot read the samples of '" +
			utf8_local( paths[index].filename() ) + "'" );
		return -1;
	}
	return max_value.load();
}


// The thumbnail to serve the overview from, for a member of `info` and type
// `type`. Among the member's own reduced pages the smallest that still resolves
// the overview (scale at most `target_scale`), else the largest - the fewest
// bytes that do the job; failing those, a same-named ".ith"/".ITH" beside it (an
// IMSTAR acquisition's thumbnail). Only thumbnails of the gallery's own pixel type
// qualify: they become a level of the same plane.
std::optional<GalleryThumbnail> choose_thumbnail_local (
	const std::filesystem::path &member_path, const std::filesystem::path &directory,
	const TiffImageInfo &info, LevelPixelType type, double target_scale )
{
	const auto *chosen = static_cast<const TiffReducedPage *>( nullptr );
	auto chosen_scale = 0.0;
	for (const auto &page : info.reduced) {
		if (page.pixel_type != type)
			continue;
		const auto scale = (double)info.size.w / page.size.w;
		const auto fits = target_scale <= 0.0 || scale <= target_scale;
		const auto chosen_fits = chosen && ( target_scale <= 0.0 || chosen_scale <= target_scale );
		const auto better = !chosen ||
			( fits && !chosen_fits ) ||
			( fits && chosen_fits && ( target_scale > 0.0 ? scale > chosen_scale : scale < chosen_scale ) ) ||
			( !fits && !chosen_fits && scale < chosen_scale );
		if (better) {
			chosen = &page;
			chosen_scale = scale;
		}
	}
	if (chosen) {
		auto thumbnail = GalleryThumbnail{};
		thumbnail.file = relative_utf8_local( member_path, directory );
		thumbnail.page = chosen->page;
		thumbnail.size = chosen->size;
		return thumbnail;
	}

	auto ec = std::error_code{};
	for (const auto *ext : { L".ith", L".ITH" }) {
		auto sidecar = member_path;
		sidecar.replace_extension( ext );
		if (!std::filesystem::is_regular_file( sidecar, ec ))
			continue;
		auto sidecar_info = TiffImageInfo{};
		if (!tiff_image_info( sidecar, &sidecar_info ) || sidecar_info.pixel_type != type ||
			sidecar_info.size.w >= info.size.w || sidecar_info.size.h >= info.size.h)
		{
			return std::nullopt;
		}
		auto thumbnail = GalleryThumbnail{};
		thumbnail.file = relative_utf8_local( sidecar, directory );
		thumbnail.page = 0;
		thumbnail.size = sidecar_info.size;
		thumbnail.file_bytes = std::filesystem::file_size( sidecar, ec );
		thumbnail.file_mtime = gallery_file_mtime( sidecar ).value_or( 0 );
		return thumbnail;
	}
	return std::nullopt;
}


// Rectangles on the grid: one per size, in order, every cell the size of the
// largest plus a gutter set in footprint pixels at the overview resolution, because
// that is the raster in which images must not touch.
struct GridPlacement {
	std::vector<Rtlbr<int>> rects;
	int columns = 0;
	int rows = 0;
	Dim2D<int> cell{ 0, 0 };
	int gutter = 0;
	double footprint_mpp = 0.0;
};


GridPlacement place_on_grid_local ( const std::vector<Dim2D<int>> &sizes, double mpp,
	float overview_mpp )
{
	auto grid = GridPlacement{};
	for (const auto &size : sizes) {
		grid.cell.w = std::max( grid.cell.w, size.w );
		grid.cell.h = std::max( grid.cell.h, size.h );
	}
	const auto count = (int)sizes.size();
	grid.columns = std::max( 1, (int)std::ceil( std::sqrt( count * kGalleryGridAspect ) ) );
	grid.rows = ( count + grid.columns - 1 ) / grid.columns;
	grid.footprint_mpp = overview_mpp;
	if (!( grid.footprint_mpp > 0.0 ))
		grid.footprint_mpp = mpp * rects_footprint_step(
			{ grid.columns * grid.cell.w, grid.rows * grid.cell.h } );
	grid.gutter = std::max( 1, (int)std::ceil(
		kGalleryGutterFootprintPx * grid.footprint_mpp / mpp ) );
	for (auto i = 0; i < count; ++i) {
		const auto x = ( i % grid.columns ) * ( grid.cell.w + grid.gutter );
		const auto y = ( i / grid.columns ) * ( grid.cell.h + grid.gutter );
		grid.rects.push_back( { { x, y }, { x + sizes[i].w, y + sizes[i].h } } );
	}
	return grid;
}


// `base`, made unique among the names `used` holds by its file-system slug.
std::string unique_zone_name_local ( const std::string &base,
	std::unordered_set<std::string> *used )
{
	auto name = base;
	for (auto n = 2; !used->insert(
			lower_ascii_copy( zone_name_slug( name ) ) ).second; ++n)
	{
		name = base + "_" + std::to_string( n );
	}
	return name;
}


// A field's own name: its stem without the channel tag and scene id of the triplet
// grammar, and without the slide's name, which every field of the set repeats.
std::string field_name_local ( const IpxField &field, const IpxLayout &layout )
{
	auto name = std::string{};
	for (const auto &channel : layout.channels) {
		auto plane = field.planes.find( channel );
		if (plane == field.planes.end())
			continue;
		const auto triplet = parse_ipx_triplet_name( plane->second.filename().string() );
		name = triplet.valid ? triplet.prefix : utf8_local( plane->second.stem() );
		break;
	}
	const auto slide = lower_ascii_copy( layout.slide_name );
	if (!slide.empty() && lower_ascii_copy( name ).starts_with( slide ) )
		name.erase( 0, slide.size() );
	while (!name.empty() && ( name.front() == '_' || name.front() == ' ' ))
		name.erase( 0, 1 );
	return name.empty() ? std::format( "{:08X}", field.scene_id ) : name;
}


struct Candidate {
	std::filesystem::path path;
	std::string name;
	TiffImageInfo info;
	IpxImdMetadata imd;
	int observed_max = 0;
};

} // namespace


void set_ipx_field_namer ( IpxFieldNamer namer )
{
	std::scoped_lock lock( ipx_field_namer_mutex_local() );
	ipx_field_namer_storage_local() = std::move( namer );
}


std::string zone_name_slug ( std::string_view zone_name )
{
	auto out = std::string{};
	out.reserve( zone_name.size() );
	for (auto ch : trim_ascii_copy( zone_name )) {
//
// replace path-reserved, file-name-unsafe, control and the sidecar delimiter
		if (ch == kZoneAssocDelimiter ||
			static_cast<unsigned char>( ch ) < 0x20 ||
			kZoneNameReservedChars.find( ch ) != std::string_view::npos ||
			kZoneSlugUnsafeChars.find( ch ) != std::string_view::npos)
		{
			ch = kZoneSlugReplacementChar;
		}
		out.push_back( ch );
	}
	return out;
}


int rects_footprint_step ( const Dim2D<int> &mosaic_size )
{
//
// A footprint is a coarse map of where the images are, not a picture of them:
// the zone bounding boxes come from its components, so it only has to be fine
// enough to keep neighbouring rectangles apart. Powers of two so a footprint
// pixel is a whole number of mosaic pixels and the rectangles stay rectangles.
	constexpr auto kMaxFootprintSpan = 2048;
	auto step = 1;
	auto span = std::max( mosaic_size.w, mosaic_size.h );
	while (span / step > kMaxFootprintSpan)
		step *= 2;
	return step;
}


std::filesystem::path gallery_manifest_path ( const std::filesystem::path &directory )
{
	auto dir = directory;
	if (!dir.has_filename())
		dir = dir.parent_path();
	auto name = dir.filename();
	name += std::filesystem::path( std::string( kGalleryExtension ) );
	return dir / name;
}


IpxGalleryFit ipx_gallery_fit ( const IpxLayout &layout )
{
	auto fit = IpxGalleryFit{};
	const auto envelope = (double)layout.size.w * layout.size.h;
	if (layout.fields.empty() || !( envelope > 0.0 )) {
		fit.why = "the set has no fields";
		return fit;
	}
	auto surface = 0.0;
	for (auto i = std::size_t{ 0 }; i < layout.fields.size(); ++i) {
		const auto &rect = layout.fields[i].rect;
		surface += (double)width( rect ) * height( rect );
		for (auto j = i + 1; j < layout.fields.size(); ++j) {
			if (overlaps( rect, layout.fields[j].rect )) {
				fit.why = "its fields overlap, so tissue in an overlap is analysed in both zones";
				return fit;
			}
		}
	}
	fit.fill_factor = surface / envelope;
	if (fit.fill_factor >= kGalleryMaxFillFactor) {
		fit.why = std::format( "its fields cover {:.0f}% of their envelope, more than the "
			"{:.0f}% of independent images", fit.fill_factor * 100.0,
			kGalleryMaxFillFactor * 100.0 );
		return fit;
	}
	fit.fits = true;
	return fit;
}


bool gallery_from_ipx_layout ( const IpxLayout &layout,
	const GalleryScanOptions &options, GalleryManifest *out,
	GalleryScanReport *report_out, std::string *error,
	const GalleryScanProgressCallback &progress )
{
	if (!out)
		return set_error_local( error, "manifest output is null" );
	auto local_report = GalleryScanReport{};
	auto &report = report_out ? *report_out : local_report;
	report = {};
	report.manifest_path = layout.directory /
		( layout.slide_name + std::string( kGalleryExtension ) );
	report.mpp_source = "imd";

	const auto fit = ipx_gallery_fit( layout );
	report.note = fit.why;

	const auto namer = ipx_field_namer_local();
	struct NamedField {
		std::string name;
		const IpxField *field = nullptr;
	};
	auto fields = std::vector<NamedField>{};
	for (const auto &field : layout.fields) {
		auto name = namer ? namer( field, layout ) : std::string{};
		if (name.empty())
			name = field_name_local( field, layout );
		fields.push_back( { zone_name_from_stem_local( name ), &field } );
	}
	std::sort( fields.begin(), fields.end(),
		[] ( const NamedField &a, const NamedField &b ) {
			return natural_less_local( a.name, b.name );
		} );

	auto sizes = std::vector<Dim2D<int>>{};
	for (const auto &entry : fields)
		sizes.push_back( { entry.field->width, entry.field->height } );
	const auto grid = place_on_grid_local( sizes, layout.mpp, options.overview_mpp );
	report.gutter_px = grid.gutter;

	auto manifest = GalleryManifest{};
	manifest.pixel_type = LevelPixelType::Gray16;
	manifest.mpp = layout.mpp;
	manifest.mpp_source = report.mpp_source;
	const auto directory = report.manifest_path.parent_path();
	auto ec = std::error_code{};
	auto used_slugs = std::unordered_set<std::string>{};
	for (auto &entry : fields)
		entry.name = unique_zone_name_local( entry.name, &used_slugs );
	auto paths = std::vector<std::filesystem::path>{};
	auto files = std::size_t{ 0 };
	for (const auto &channel : layout.channels) {
		for (const auto &entry : fields)
			files += entry.field->planes.contains( channel ) ? 1 : 0;
	}
	const auto total_units = 2 * files;
	for (const auto &channel : layout.channels) {
		for (auto i = std::size_t{ 0 }; i < fields.size(); ++i) {
			const auto &field = *fields[i].field;
			auto plane = field.planes.find( channel );
			if (plane == field.planes.end())
				continue;
			const auto &path = plane->second;
			if (progress)
				progress( (std::size_t)report.candidates, total_units );
			++report.candidates;
			auto info = TiffImageInfo{};
			auto reason = std::string{};
			if (!tiff_image_info( path, &info, &reason ))
				return set_error_local( error, "'" + utf8_local( path.filename() ) + "': " + reason );
			if (info.pixel_type != LevelPixelType::Gray16 ||
				!are_wh_eq( info.size, Dim2D{ field.width, field.height } ))
			{
				return set_error_local( error, "'" + utf8_local( path.filename() ) +
					"' is not the 16-bit grey raster its .IMD describes" );
			}
			auto member = GalleryMember{};
			member.file = relative_utf8_local( path, directory );
			member.zone = fields[i].name;
			member.channel = channel;
			member.rect = grid.rects[i];
			member.file_bytes = std::filesystem::file_size( path, ec );
			member.file_mtime = gallery_file_mtime( path ).value_or( 0 );
			member.stage_um = Pxy<double>{ field.origin_x_um, field.origin_y_um };
			member.thumbnail = choose_thumbnail_local( path, directory, info,
				manifest.pixel_type, grid.footprint_mpp / manifest.mpp );
			manifest.members.push_back( std::move( member ) );
			paths.push_back( path );
			report.uncompressed += info.rewritable ? 1 : 0;
		}
	}

//
// The range comes from the samples, as the converter's does: the layout's is only
// the 12-bit contract, and a set of byte samples held in 16-bit words would come
// up near black against it. Each channel has its own measured full scale.
	const auto range_started = std::chrono::steady_clock::now();
	auto measured = std::size_t{ 0 };
	for (const auto &channel : layout.channels) {
		auto channel_paths = std::vector<std::filesystem::path>{};
		for (auto i = std::size_t{ 0 }; i < manifest.members.size(); ++i) {
			if (manifest.members[i].channel == channel)
				channel_paths.push_back( paths[i] );
		}
		const auto observed = observed_max_local<uint16_t>( channel_paths, kIpxByteRange,
			error, [&] ( std::size_t read ) {
				if (progress)
					progress( files + measured + read, total_units );
			} );
		if (observed < 0)
			return false;
		const auto scale = ipx_channel_range( observed > kIpxByteRange );
		manifest.channel_max_pixel_values.emplace( channel, scale );
		manifest.max_pixel_value = std::max( manifest.max_pixel_value, scale );
		measured += channel_paths.size();
	}
	pyr::plog( "gallery: '%s' - sample range 0..%d, %zu file(s) in %s\n",
		layout.slide_name.c_str(), manifest.max_pixel_value, paths.size(),
		std::format( "{:.1f} ms", std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - range_started ).count() ).c_str() );

	report.accepted = (int)fields.size();
	auto thumbnail_why = std::string{};
	const auto scale = gallery_thumbnail_scale( manifest, &thumbnail_why );
	pyr::plog( "gallery: '%s' - %zu field(s), %zu channel(s), %d file(s), fill %.1f%%, "
		"%.6g um/px, grid %dx%d, cell %dx%d, gutter %d px, %s%s\n",
		layout.slide_name.c_str(), fields.size(), layout.channels.size(),
		report.candidates, fit.fill_factor * 100.0, manifest.mpp, grid.columns, grid.rows,
		grid.cell.w, grid.cell.h, grid.gutter,
		scale ? "overview from stored thumbnails" : "overview derived from the members: ",
		scale ? "" : thumbnail_why.c_str() );
	if (!validate_gallery_manifest( manifest, error ))
		return false;
	if (progress)
		progress( total_units, total_units );
	*out = std::move( manifest );
	return true;
}


bool scan_gallery_directory ( const std::filesystem::path &directory,
	const GalleryScanOptions &options, GalleryManifest *out,
	GalleryScanReport *report_out, std::string *error,
	const GalleryScanProgressCallback &progress )
{
	if (!out)
		return set_error_local( error, "manifest output is null" );
	auto local_report = GalleryScanReport{};
	auto &report = report_out ? *report_out : local_report;
	report = {};
	auto ec = std::error_code{};
	if (!std::filesystem::is_directory( directory, ec ))
		return set_error_local( error, "'" + utf8_local( directory ) + "' is not a directory" );

//
// An IPX set is one slide's fields, not a list of images, and a set the scan
// refuses is refused here for the same reason.
	auto ipx = IpxLayout{};
	auto ipx_error = std::string{};
	const auto ipx_scanned = scan_ipx_file_set( directory, {}, &ipx, &ipx_error );
	if (ipx.members_present) {
		if (!ipx_scanned)
			return set_error_local( error, ipx_error );
		return gallery_from_ipx_layout( ipx, options, out, &report, error, progress );
	}

	const auto manifest_path = gallery_manifest_path( directory );
	report.manifest_path = manifest_path;
	const auto own_prefix = utf8_local( manifest_path.stem() ) + kZoneAssocDelimiter;

	auto tiffs = std::vector<std::filesystem::path>{};
	for (const auto &entry : std::filesystem::directory_iterator( directory, ec )) {
		if (!entry.is_regular_file( ec ) || !is_tiff_extension_local( entry.path() ))
			continue;
//
// What this gallery itself writes beside its manifest - zone and overview
// sidecars, "{stem}~..." - is never a member.
		if (!utf8_local( entry.path().filename() ).starts_with( own_prefix ))
			tiffs.push_back( entry.path() );
	}
	if (ec)
		return set_error_local( error, "cannot list '" + utf8_local( directory ) + "'" );

//
// Every file is expected to need its samples read until the headers say otherwise.
	const auto files = tiffs.size();
	auto total_units = files;
	auto candidates = std::vector<Candidate>{};
	for (const auto &path : tiffs) {
		if (progress)
			progress( (std::size_t)report.candidates, 2 * files );
		const auto name = utf8_local( path.filename() );
		++report.candidates;
		auto candidate = Candidate{ path, name, {}, {} };
		auto reason = std::string{};
		if (!tiff_image_info( path, &candidate.info, &reason )) {
			report.refused.emplace_back( name, reason );
			continue;
		}
//
// More pages than the image and its reduced copies - other channels, other focal
// planes - are not one cell image.
		if (candidate.info.other_pages > 0) {
			report.refused.emplace_back( name, "multi-page TIFF" );
			continue;
		}
		load_imd_metadata_beside( path, &candidate.imd );
		candidates.push_back( std::move( candidate ) );
	}

// Decode before choosing the majority or calibration, so unreadable files can
// neither decide the gallery's type nor make valid members disagree on mpp.
	auto measured = std::size_t{ 0 };
	for (const auto type : { LevelPixelType::Gray8, LevelPixelType::Gray16,
		LevelPixelType::Rgb8, LevelPixelType::Rgb16 })
	{
		auto paths = std::vector<std::filesystem::path>{};
		auto indices = std::vector<std::size_t>{};
		for (auto i = std::size_t{ 0 }; i < candidates.size(); ++i) {
			if (candidates[i].info.pixel_type == type) {
				paths.push_back( candidates[i].path );
				indices.push_back( i );
			}
		}
		auto maxima = std::vector<int>{};
		visit_level_pixel_type( type, [&] ( auto pixel ) {
			observed_max_local<decltype( pixel )>( paths, std::numeric_limits<uint16_t>::max(), nullptr,
				[&] ( std::size_t read ) {
					if (progress)
						progress( files + measured + read, 2 * files );
				}, &maxima );
		} );
		for (auto i = std::size_t{ 0 }; i < indices.size(); ++i)
			candidates[indices[i]].observed_max = maxima[i];
		measured += paths.size();
	}
	std::erase_if( candidates, [&] ( const Candidate &candidate ) {
		if (candidate.observed_max >= 0)
			return false;
		report.refused.emplace_back( candidate.name, "image samples could not be decoded" );
		return true;
	} );
	total_units = 2 * files;

//
// One pixel type per gallery: the majority's. A stray thumbnail in colour among
// grey cells is reported, not allowed to decide the type of the slide.
	auto by_type = std::map<LevelPixelType, int>{};
	for (const auto &candidate : candidates)
		++by_type[candidate.info.pixel_type];
	if (by_type.empty()) {
		return set_error_local( error, "no readable single-page TIFF in '" +
			utf8_local( directory ) + "'" );
	}
	const auto type = std::max_element( by_type.begin(), by_type.end(),
		[] ( const auto &a, const auto &b ) { return a.second < b.second; } )->first;
	std::erase_if( candidates, [&] ( const Candidate &candidate ) {
		if (candidate.info.pixel_type == type)
			return false;
		report.refused.emplace_back( candidate.name, std::string( "pixel type " ) +
			pixel_type_name( candidate.info.pixel_type ) + ", the gallery is " +
			pixel_type_name( type ) );
		return true;
	} );
	std::sort( candidates.begin(), candidates.end(),
		[] ( const Candidate &a, const Candidate &b ) {
			return natural_less_local( a.name, b.name );
		} );

//
// Pixel size: every member that states one must agree; members that state none
// are taken to share it. Only when none states one does the caller's value apply.
	auto stated = std::optional<StatedMpp>{};
	auto stating = 0;
	for (const auto &candidate : candidates) {
		auto mpp = member_mpp_local( candidate.info, candidate.imd );
		if (!mpp)
			continue;
		++stating;
		if (!stated) {
			stated = mpp;
			continue;
		}
		if (!same_mpp_local( stated->mpp, mpp->mpp ))
			return set_error_local( error, "members disagree on their pixel size: " +
				std::to_string( stated->mpp ) + " um (" + stated->source + ") and " +
				std::to_string( mpp->mpp ) + " um (" + mpp->source + ", '" +
				candidate.name + "') - one gallery holds one resolution" );
	}
	auto manifest = GalleryManifest{};
	manifest.pixel_type = type;
	if (stated) {
		manifest.mpp = stated->mpp;
		manifest.mpp_source = stated->source;
		if (stating < (int)candidates.size())
			pyr::plog( "gallery: '%s' - %d of %zu member(s) state %.6g um/px (%s); "
				"the rest are taken to match\n", utf8_local( directory ).c_str(),
				stating, candidates.size(), manifest.mpp, stated->source.c_str() );
	}
	else if (options.mpp > 0.0) {
		manifest.mpp = options.mpp;
		manifest.mpp_source = "user";
	}
	else {
		report.needs_mpp = true;
		return set_error_local( error, "no member states its pixel size (no .imd, "
			"no microscope resolution tags); give the microns per pixel" );
	}

//
// A grey gallery's plane needs a name; the acquisition's own mode name when every
// .imd agrees on one, the default image plane otherwise.
	auto channel = std::string{};
	if (is_integer_gray( type )) {
		auto modes = std::unordered_set<std::string>{};
		for (const auto &candidate : candidates)
			modes.insert( candidate.imd.loaded ? candidate.imd.plane_name : std::string{} );
		channel = modes.size() == 1 && !modes.begin()->empty()
			? canonical_ipx_plane_name( *modes.begin(), -1 )
			: std::string( kDefaultImagePlaneName );
	}
	if (type == LevelPixelType::Gray16 || type == LevelPixelType::Rgb16) {
		auto observed = 0;
		for (const auto &candidate : candidates)
			observed = std::max( observed, candidate.observed_max );
		manifest.max_pixel_value = bit_depth_full_scale( std::max( observed, kIpxByteRange ) );
	}

	auto sizes = std::vector<Dim2D<int>>{};
	for (const auto &candidate : candidates)
		sizes.push_back( candidate.info.size );
	const auto grid = place_on_grid_local( sizes, manifest.mpp, options.overview_mpp );
	report.gutter_px = grid.gutter;

	const auto count = (int)candidates.size();
	auto used_slugs = std::unordered_set<std::string>{};
	for (auto i = 0; i < count; ++i) {
		const auto &candidate = candidates[i];
		auto member = GalleryMember{};
		member.file = candidate.name;
		member.channel = channel;
		member.rect = grid.rects[i];
		member.file_bytes = std::filesystem::file_size( candidate.path, ec );
		member.file_mtime = gallery_file_mtime( candidate.path ).value_or( 0 );
		if (candidate.imd.loaded)
			member.stage_um = Pxy<double>{ candidate.imd.origin_x_um, candidate.imd.origin_y_um };
		member.thumbnail = choose_thumbnail_local( candidate.path, directory, candidate.info,
			type, grid.footprint_mpp / manifest.mpp );
		member.zone = unique_zone_name_local(
			zone_name_from_stem_local( utf8_local( candidate.path.stem() ) ), &used_slugs );
		manifest.members.push_back( std::move( member ) );
		report.uncompressed += candidate.info.rewritable ? 1 : 0;
	}
	report.accepted = count;
	auto thumbnail_why = std::string{};
	if (const auto scale = gallery_thumbnail_scale( manifest, &thumbnail_why ))
		pyr::plog( "gallery: '%s' - every member has a stored thumbnail at 1/%.3f; the "
			"overview is built from them\n", utf8_local( directory ).c_str(), *scale );
	else
		pyr::plog( "gallery: '%s' - overview derived from the members: %s\n",
			utf8_local( directory ).c_str(), thumbnail_why.c_str() );
	report.mpp_source = manifest.mpp_source;
	pyr::plog( "gallery: '%s' - %d member(s) of %d candidate(s), %s, %.6g um/px (%s), "
		"grid %dx%d, cell %dx%d, gutter %d px\n", utf8_local( directory ).c_str(),
		count, report.candidates, pixel_type_name( type ), manifest.mpp,
		manifest.mpp_source.c_str(), grid.columns, grid.rows, grid.cell.w, grid.cell.h,
		grid.gutter );
	for (const auto &[name, reason] : report.refused)
		pyr::plog( "gallery: '%s' refused: %s\n", name.c_str(), reason.c_str() );

	if (!validate_gallery_manifest( manifest, error ))
		return false;
	if (progress)
		progress( total_units, total_units );
	*out = std::move( manifest );
	return true;
}


} // namespace priml
