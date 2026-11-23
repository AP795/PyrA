// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "web_server.h"
#include "web_analyse.h"
#include "web_assets.h"

#include <httplib.h>
#include <pyra/pyra.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(CPPHTTPLIB_OPENSSL_SUPPORT) || defined(CPPHTTPLIB_ZLIB_SUPPORT) || defined(CPPHTTPLIB_BROTLI_SUPPORT)
#error The standalone localhost viewer must not enable optional HTTP SDKs.
#endif

namespace pyra::web {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kMaxDirectoryEntries = size_t{ 512 };
constexpr auto kBytesPerMiB = uint64_t{ 1024 } * 1024;

std::string utf8 ( const std::filesystem::path &path )
{
	const auto bytes = path.generic_u8string();
	return { bytes.begin(), bytes.end() };
}


void json ( httplib::Response &response, const std::string &value )
{
	response.set_content( value, "application/json; charset=utf-8" );
}


void fail ( httplib::Response &response, int status, const std::string &message )
{
	response.status = status;
	json( response, "{\"error\":" + json_string( message ) + "}" );
}


double parameter ( const httplib::Request &request, const char *name )
{
	const auto value = request.get_param_value( name );
	auto number = 0.0;
	const auto parsed = std::from_chars( value.data(), value.data() + value.size(), number );
	if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite( number ))
		throw std::invalid_argument( std::format( "Invalid parameter: {}", name ) );
	return number;
}


struct CodecName {
	pyra_codec codec;
	const char *name;
	bool lossless;
};

constexpr CodecName kCodecs[] = {
	{ PYRA_CODEC_JPEG, "JPEG", false },
	{ PYRA_CODEC_JPEG2000, "JPEG 2000", true },
	{ PYRA_CODEC_HTJ2K, "HTJ2K", true },
	{ PYRA_CODEC_JPEGXL, "JPEG XL", true },
};


// The codecs this build can write, with the synthetic-slide defaults and limits.
std::string synthetic_json ()
{
	auto defaults = pyra_synthetic_options{};
	pyra_synthetic_options_init( &defaults );
	auto codecs = std::string( "[" );
	for (const auto &codec : kCodecs) {
		if (!pyra_codec_can_encode( codec.codec ))
			continue;
		if (codecs.size() > 1) codecs += ',';
		codecs += std::format( "{{\"id\":{},\"name\":{},\"lossless\":{},\"dicom\":{}}}", codec.codec,
			json_string( codec.name ), codec.lossless, pyra_synthetic_dicom_can_encode( codec.codec ) != 0 );
	}
	return std::format( "{{\"codecs\":{}],\"width\":{},\"height\":{},\"tile\":{},\"z\":{},\"mpp\":{},"
		"\"codec\":{},\"quality\":{},\"tile_min\":{},\"tile_max\":{},\"tile_step\":{},\"edge_max\":{},"
		"\"z_max\":{},\"mpp_min\":{},\"mpp_max\":{},\"z_step\":{},\"z_step_min\":{},\"z_step_max\":{},"
		"\"focus\":{},\"nucleus\":{},\"nucleus_min\":{},\"nucleus_max\":{},\"spacing\":{},\"spacing_min\":{},"
		"\"spacing_max\":{},\"positive\":{},\"tumour\":{},\"lymphocytes\":{}}}", codecs, defaults.width,
		defaults.height, defaults.tile_edge, defaults.z_levels, defaults.mpp, defaults.codec, defaults.quality,
		PYRA_SYNTHETIC_TILE_MIN, PYRA_SYNTHETIC_TILE_MAX, PYRA_SYNTHETIC_TILE_STEP, PYRA_SYNTHETIC_EDGE_MAX,
		PYRA_SYNTHETIC_Z_MAX, PYRA_SYNTHETIC_MPP_MIN, PYRA_SYNTHETIC_MPP_MAX, defaults.z_step_um,
		PYRA_SYNTHETIC_Z_STEP_MIN_UM, PYRA_SYNTHETIC_Z_STEP_MAX_UM, defaults.focus_fraction,
		defaults.nucleus_diameter_um, PYRA_SYNTHETIC_NUCLEUS_MIN_UM, PYRA_SYNTHETIC_NUCLEUS_MAX_UM,
		defaults.nucleus_spacing_um, PYRA_SYNTHETIC_SPACING_MIN_UM, PYRA_SYNTHETIC_SPACING_MAX_UM,
		defaults.positive_fraction, defaults.tumour_fraction, defaults.lymphocyte_fraction );
}


// A bare file name for a generated slide with the selected format's extension.
std::filesystem::path synthetic_file_name ( const std::string &name, const std::string &format )
{
	const auto file = utf8_path( name );
	if (name.empty() || name.find_first_of( "/\\:" ) != std::string::npos || name.find( '\0' ) != std::string::npos ||
		file != file.filename() || file == "." || file == "..")
	{
		throw std::invalid_argument( "Name the slide with a plain file name" );
	}
	auto extension = utf8( file.extension() );
	std::transform( extension.begin(), extension.end(), extension.begin(),
		[] ( unsigned char ch ) { return static_cast<char>( std::tolower( ch ) ); } );
	const auto desired = format == "dicom" ? ".dcm" : ".ptf";
	if (extension == desired)
		return file;
	if (!format.empty() && ( extension == ".dcm" || extension == ".ptf" ))
		return std::filesystem::path( file ).replace_extension( desired );
	return std::filesystem::path( file ).concat( desired );
}


uint64_t integer ( const httplib::Request &request, const char *name, uint64_t maximum )
{
	const auto value = request.get_param_value( name );
	auto number = uint64_t{};
	const auto parsed = std::from_chars( value.data(), value.data() + value.size(), number );
	if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || number > maximum)
		throw std::invalid_argument( std::format( "Invalid parameter: {}", name ) );
	return number;
}


struct Session {
	pyra_context *context_ = nullptr;
	pyra_wsi *wsi_ = nullptr;
	std::filesystem::path path_;		// absolute
	std::string relative_;				// as the client named it
	std::vector<pyra_plane_info> planes_;
	~Session () {
		if (wsi_)
			pyra_close( wsi_, nullptr );
		if (context_)
			pyra_context_destroy( context_, nullptr );
	}
};


uint8_t display_sample ( const unsigned char *row, size_t index, pyra_pixel_format format, double range )
{
	auto sample = 0.0;
	if (format == PYRA_PIXEL_RGB16 || format == PYRA_PIXEL_GRAY16) {
		auto value = uint16_t{};
		std::memcpy( &value, row + index * sizeof( value ), sizeof( value ) );
		sample = value;
	} else if (format == PYRA_PIXEL_FLOAT32) {
		auto value = float{};
		std::memcpy( &value, row + index * sizeof( value ), sizeof( value ) );
		sample = value;
	} else {
		sample = row[index];
	}
	const auto scaled = sample * 255.0 / range;
	return std::isfinite( scaled ) ? uint8_t( std::clamp( scaled, 0.0, 255.0 ) ) : 0;
}


std::string rgba_pixels ( const std::vector<unsigned char> &source, const pyra_image_layout &layout, double range )
{
	auto result = std::string( size_t( layout.width ) * layout.height * 4, '\0' );
	const auto rgb = layout.pixel_format == PYRA_PIXEL_RGB8 || layout.pixel_format == PYRA_PIXEL_RGB16;
	if (!std::isfinite( range ) || range <= 0)
		range = 1;
	for (auto y = 0u; y < layout.height; ++y) {
		const auto *row = source.data() + size_t( y ) * layout.row_stride_bytes;
		auto *out = reinterpret_cast<unsigned char *>( result.data() + size_t( y ) * layout.width * 4 );
		for (auto x = 0u; x < layout.width; ++x) {
			for (auto c = 0u; c < 3; ++c)
				out[x * 4 + c] = display_sample( row, rgb ? size_t( x ) * 3 + c : x, layout.pixel_format, range );
			out[x * 4 + 3] = 255;
		}
	}
	return result;
}

} // namespace

class Server::Impl {
public:
	explicit Impl ( ServerOptions options ) : options_( std::move( options ) ), analyser_( options_ ) {
		options_.root = std::filesystem::canonical( options_.root );
		if (!std::filesystem::is_directory( options_.root ))
			throw std::invalid_argument( "The image root must be a directory" );
		auto random = std::random_device{};
		for (auto i = 0; i < 4; ++i)
			token_ += std::format( "{:08x}", random() );
		http_.new_task_queue = [] { return new httplib::ThreadPool( 2, 0, 4 ); };
		http_.set_payload_max_length( 8192 );
		http_.set_read_timeout( 5, 0 );
		http_.set_write_timeout( 30, 0 );
		http_.set_keep_alive_max_count( 20 );
		http_.set_logger( [this] ( const auto &request, const auto &response ) {
			if (options_.log) options_.log( std::format( "{} {} status={}", request.method, request.path, response.status ) );
		} );
		http_.set_pre_routing_handler( [this] ( const auto &request, auto &response ) {
			response.set_header( "Cache-Control", "no-store" );
			response.set_header( "X-Content-Type-Options", "nosniff" );
			response.set_header( "Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' blob:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'" );
			const auto host = std::format( "127.0.0.1:{}", port_ );
			const auto origin = request.get_header_value( "Origin" );
			if (request.get_header_value( "Host" ) != host || ( !origin.empty() && origin != "http://" + host )) {
				fail( response, 403, "Use the printed loopback URL" );
				return httplib::Server::HandlerResponse::Handled;
			}
			if (request.path.starts_with( "/api/" ) && request.get_header_value( "X-Pyra-Session" ) != token_) {
				fail( response, 403, "Reload the viewer page to connect to this server session" );
				return httplib::Server::HandlerResponse::Handled;
			}
			return httplib::Server::HandlerResponse::Unhandled;
		} );
		http_.set_exception_handler( [this] ( const auto &, auto &response, std::exception_ptr exception ) {
			try { if (exception) std::rethrow_exception( exception ); }
			catch (const std::invalid_argument &error) { fail( response, 400, error.what() ); }
			catch (const std::exception &error) { fail( response, 500, error.what() ); }
			catch (...) { fail( response, 500, "Unexpected server error" ); }
		} );
		http_.Get( "/", [this] ( const auto &, auto &response ) {
			auto html = std::string( assets::html );
			const auto marker = html.find( "PYRA_SESSION_TOKEN" );
			html.replace( marker, std::strlen( "PYRA_SESSION_TOKEN" ), token_ );
			response.set_content( std::move( html ), "text/html; charset=utf-8" );
		} );
		http_.Get( "/viewer.css", [] ( const auto &, auto &response ) {
			response.set_content( assets::css.data(), assets::css.size(), "text/css; charset=utf-8" );
		} );
		http_.Get( "/viewer.js", [] ( const auto &, auto &response ) {
			response.set_content( assets::js.data(), assets::js.size(), "text/javascript; charset=utf-8" );
		} );
		http_.Get( "/api/state", [this] ( const auto &, auto &response ) { state( response ); } );
		http_.Get( "/api/files", [this] ( const auto &request, auto &response ) { files( request, response ); } );
		http_.Post( "/api/open", [this] ( const auto &request, auto &response ) { open( request, response ); } );
		http_.Post( "/api/make-gallery", [this] ( const auto &request, auto &response ) { make_gallery( request, response ); } );
		http_.Get( "/api/render", [this] ( const auto &request, auto &response ) { render( request, response ); } );
		http_.Post( "/api/generate", [this] ( const auto &request, auto &response ) { generate( request, response ); } );
		http_.Get( "/api/generate", [this] ( const auto &, auto &response ) { generate_status( response ); } );
		http_.Post( "/api/generate-cancel", [this] ( const auto &, auto &response ) {
			auto lock = std::lock_guard( generate_mutex_ );
			generator_.request_stop();
			generate_status_locked( response );
		} );
		http_.Get( "/api/scripts", [this] ( const auto &, auto &response ) { json( response, analyser_.scripts_json() ); } );
		http_.Post( "/api/analyse", [this] ( const auto &request, auto &response ) { analyse( request, response ); } );
		http_.Get( "/api/analyse", [this] ( const auto &, auto &response ) { json( response, analyser_.status_json() ); } );
	}

	// Starts the Python bridge on the open slide; progress and the report come
	// from GET /api/analyse. The client reopens the slide when it succeeds.
	void analyse ( const httplib::Request &request, httplib::Response &response ) {
		auto slide = std::filesystem::path{};
		auto relative = std::string{};
		{
			auto lock = std::lock_guard( mutex_ );
			if (!session_) { fail( response, 409, "Open a slide first" ); return; }
			slide = session_->path_;
			relative = session_->relative_;
		}
		auto body = std::string{};
		if (!analyser_.start( request, slide, relative, &body )) {
			fail( response, 409, "An analysis is already running" );
			return;
		}
		json( response, body );
	}

	std::filesystem::path resolve ( const std::string &relative ) const {
		const auto input = utf8_path( relative );
		if (input.is_absolute() || input.has_root_name() || relative.find( '\0' ) != std::string::npos)
			throw std::invalid_argument( "Use a path relative to the configured image root" );
		auto error = std::error_code{};
		const auto path = std::filesystem::canonical( options_.root / input, error );
		if (error)
			throw std::invalid_argument( "Image or directory does not exist beneath the image root" );
		auto parent = options_.root.begin();
		auto child = path.begin();
		for (; parent != options_.root.end(); ++parent, ++child) {
			if (child == path.end() || *parent != *child)
				throw std::invalid_argument( "Path is outside the configured image root" );
		}
		return path;
	}

	void state ( httplib::Response &response ) const {
		auto defaults = pyra_context_options{};
		pyra_context_options_init( &defaults );
		auto backend = pyra_backend_info{};
		backend.struct_size = sizeof( backend );
		pyra_get_backend_info( PYRA_BACKEND_OPENSLIDE, &backend, nullptr );
		json( response, "{\"root\":" + json_string( utf8( options_.root ) ) +
			",\"initial_image\":" + json_string( options_.initial_image ) +
			std::format( ",\"max_edge\":{},\"overview_edge\":{},\"openslide\":{},\"tbf_mib\":{},\"pbf_mib\":{},\"threads\":{},\"synthetic\":{},\"analyse\":{} }}",
				kMaxRenderEdge, kOverviewEdge, backend.available != 0, defaults.compressed_cache_bytes / kBytesPerMiB,
				defaults.decoded_cache_bytes / kBytesPerMiB, defaults.decode_threads, synthetic_json(), analyser_.capabilities_json() ) );
	}

	// Starts writing a synthetic slide into a folder beneath the root; progress
	// and the outcome come from GET /api/generate. An existing file is refused.
	void generate ( const httplib::Request &request, httplib::Response &response ) {
		const auto directory = resolve( request.get_param_value( "path" ) );
		if (!std::filesystem::is_directory( directory ))
			throw std::invalid_argument( "Choose a directory" );
		const auto format = request.get_param_value( "format" );
		if (request.has_param( "format" ) && format != "ptf" && format != "dicom")
			throw std::invalid_argument( "Choose PTF or DICOM" );
		const auto dicom = format == "dicom";
		const auto target = directory / synthetic_file_name( request.get_param_value( "name" ), format );
		auto options = pyra_synthetic_options{};
		pyra_synthetic_options_init( &options );
		options.width = uint32_t( integer( request, "width", PYRA_SYNTHETIC_EDGE_MAX ) );
		options.height = uint32_t( integer( request, "height", PYRA_SYNTHETIC_EDGE_MAX ) );
		options.tile_edge = uint32_t( integer( request, "tile", PYRA_SYNTHETIC_TILE_MAX ) );
		options.z_levels = uint32_t( integer( request, "z", PYRA_SYNTHETIC_Z_MAX ) );
		options.mpp = parameter( request, "mpp" );
		options.codec = pyra_codec( integer( request, "codec", UINT32_MAX ) );
		if (dicom && !pyra_synthetic_dicom_can_encode( options.codec ))
			throw std::invalid_argument( "DICOM generation needs DCMTK and an enabled JPEG, JPEG 2000 or HTJ2K encoder" );
		options.quality = request.get_param_value( "lossless" ) == "1" ? PYRA_QUALITY_LOSSLESS
			: uint32_t( integer( request, "quality", PYRA_QUALITY_LOSSLESS ) );
// Focus and tissue parameters keep the library defaults when absent; the
// library validates their ranges.
		const std::pair<const char *, double *> reals[] = {
			{ "z_step", &options.z_step_um }, { "focus", &options.focus_fraction },
			{ "nucleus", &options.nucleus_diameter_um }, { "spacing", &options.nucleus_spacing_um },
			{ "positive", &options.positive_fraction }, { "tumour", &options.tumour_fraction },
			{ "lymphocytes", &options.lymphocyte_fraction } };
		for (const auto &[name, field] : reals) {
			if (request.has_param( name ))
				*field = parameter( request, name );
		}
		auto lock = std::lock_guard( generate_mutex_ );
		if (synthetic_.running) { fail( response, 409, "A slide is already being generated" ); return; }
		auto error = std::error_code{};
		if (std::filesystem::exists( target, error )) {
			fail( response, 409, std::format( "{} already exists; choose another name", utf8( target.filename() ) ) );
			return;
		}
		if (generator_.joinable())
			generator_.join();
		synthetic_ = Generation{ true, 0, 0, {}, utf8( target.lexically_relative( options_.root ) ) };
		generator_ = std::jthread( [this, options, target, dicom] ( std::stop_token stop ) mutable {
			struct Progress { Impl *impl; std::stop_token stop; } progress{ this, stop };
			options.progress_user = &progress;
			options.progress = [] ( void *user, uint64_t done, uint64_t total ) -> int {
				auto *state = static_cast<Progress *>( user );
				auto lock = std::lock_guard( state->impl->generate_mutex_ );
				state->impl->synthetic_.done = done;
				state->impl->synthetic_.total = total;
				return state->stop.stop_requested() ? 0 : 1;
			};
			auto failure = pyra_error{};
			const auto write = dicom ? pyra_make_synthetic_dicom : pyra_make_synthetic;
			const auto status = write( &options, utf8( target ).c_str(), &failure );
			auto lock = std::lock_guard( generate_mutex_ );
			synthetic_.running = false;
			if (status != PYRA_OK)
				synthetic_.error = failure.message;
		} );
		generate_status_locked( response );
	}

	void generate_status ( httplib::Response &response ) {
		auto lock = std::lock_guard( generate_mutex_ );
		generate_status_locked( response );
	}

	void generate_status_locked ( httplib::Response &response ) const {
		json( response, std::format( "{{\"running\":{},\"done\":{},\"total\":{},\"error\":{},\"path\":{}}}",
			synthetic_.running, synthetic_.done, synthetic_.total, json_string( synthetic_.error ),
			json_string( synthetic_.path ) ) );
	}

	void files ( const httplib::Request &request, httplib::Response &response ) const {
		const auto directory = resolve( request.get_param_value( "path" ) );
		if (!std::filesystem::is_directory( directory ))
			throw std::invalid_argument( "Choose a directory" );
		auto entries = std::vector<std::pair<std::string, bool>>{};
		auto truncated = false;
		for (const auto &entry : std::filesystem::directory_iterator( directory )) {
			if (entries.size() >= kMaxDirectoryEntries) { truncated = true; break; }
			auto error = std::error_code{};
			if (entry.is_directory( error ) || entry.is_regular_file( error ))
				entries.emplace_back( utf8( entry.path().filename() ), entry.is_directory( error ) );
		}
		std::sort( entries.begin(), entries.end(), [] ( const auto &left, const auto &right ) {
			return left.second != right.second ? left.second > right.second : left.first < right.first;
		} );
		auto result = "{\"path\":" + json_string( utf8( directory.lexically_relative( options_.root ) ) ) + ",\"entries\":[";
		for (const auto &[name, folder] : entries) {
			if (result.back() != '[') result += ',';
			result += "{\"name\":" + json_string( name ) + ",\"directory\":" + ( folder ? "true}" : "false}" );
		}
		json( response, result + "],\"truncated\":" + ( truncated ? "true}" : "false}" ) );
	}

	void make_gallery ( const httplib::Request &request, httplib::Response &response ) const {
		const auto directory = resolve( request.get_param_value( "path" ) );
		if (!std::filesystem::is_directory( directory ))
			throw std::invalid_argument( "Choose a directory" );
		const auto mpp = request.has_param( "mpp" ) ? std::stod( request.get_param_value( "mpp" ) ) : 0.0;
		char manifest[4096] = {};
		auto error = pyra_error{};
		if (pyra_make_gallery( utf8( directory ).c_str(), mpp, manifest, sizeof( manifest ), &error ) != PYRA_OK) {
			fail( response, 400, error.message );
			return;
		}
		const auto made = std::filesystem::path( std::u8string( reinterpret_cast<const char8_t *>( manifest ) ) );
		json( response, "{\"path\":" + json_string( utf8( made.lexically_relative( options_.root ) ) ) + "}" );
	}

	void open ( const httplib::Request &request, httplib::Response &response ) {
		auto lock = std::unique_lock( mutex_, std::try_to_lock );
		if (!lock.owns_lock()) { fail( response, 409, "Renderer busy; try opening again" ); return; }
		const auto relative = request.get_param_value( "path" );
		if (relative.empty()) throw std::invalid_argument( "Choose an image" );
		const auto path = resolve( relative );
		auto options = pyra_context_options{};
		pyra_context_options_init( &options );
		options.max_image_pixels = uint64_t( kMaxRenderEdge ) * kMaxRenderEdge;
		if (request.has_param( "backend" )) options.backend = uint32_t( integer( request, "backend", PYRA_BACKEND_OPENSLIDE ) );
		if (options.backend == PYRA_BACKEND_NATIVE && request.has_param( "tbf_mib" ))
			options.compressed_cache_bytes = integer( request, "tbf_mib", 1048576 ) * kBytesPerMiB;
		if (request.has_param( "pbf_mib" )) options.decoded_cache_bytes = integer( request, "pbf_mib", 1048576 ) * kBytesPerMiB;
		if (options.backend == PYRA_BACKEND_NATIVE && request.has_param( "threads" ))
			options.decode_threads = uint32_t( integer( request, "threads", 256 ) );
		auto next = std::make_unique<Session>();
		next->path_ = path;
		next->relative_ = relative;
		auto error = pyra_error{};
		const auto start = Clock::now();
		if (pyra_context_create( &options, &next->context_, &error ) != PYRA_OK ||
			pyra_open( next->context_, utf8( path ).c_str(), &next->wsi_, &error ) != PYRA_OK) {
			fail( response, 400, error.message );
			return;
		}
		auto count = uint32_t{};
		if (pyra_get_plane_count( next->wsi_, &count, &error ) != PYRA_OK)
			throw std::runtime_error( error.message );
		auto planes = std::string( "[" );
		for (auto i = 0u; i < count; ++i) {
			auto plane = pyra_plane_info{};
			plane.struct_size = sizeof( plane );
			if (pyra_get_plane_info( next->wsi_, i, &plane, &error ) != PYRA_OK)
				throw std::runtime_error( error.message );
			next->planes_.push_back( plane );
			if (i) planes += ',';
			const auto z_is_index = !std::isfinite( plane.z_um );
			planes += "{\"channel\":" + json_string( plane.channel ? plane.channel : "" ) +
				std::format( ",\"index\":{},\"z\":{},\"z_is_index\":{},\"mpp\":{},\"x\":{},\"y\":{},\"width\":{},\"height\":{},\"format\":{} }}",
					i, z_is_index ? double( i ) : plane.z_um, z_is_index, plane.native_mpp, plane.bounds.x_um, plane.bounds.y_um,
					plane.bounds.width_um, plane.bounds.height_um, plane.pixel_format );
		}
		if (count == 0) throw std::invalid_argument( "No image planes were found" );
		auto properties = std::string( "[" );
		if (pyra_get_property_count( next->wsi_, &count, &error ) != PYRA_OK)
			throw std::runtime_error( error.message );
		for (auto i = 0u; i < count; ++i) {
			auto property = pyra_property{};
			if (pyra_get_property( next->wsi_, i, &property, &error ) != PYRA_OK)
				throw std::runtime_error( error.message );
			if (i) properties += ',';
			properties += '[' + json_string( property.key ) + ',' + json_string( property.value ) + ']';
		}
		const auto elapsed = std::chrono::duration<double, std::milli>( Clock::now() - start ).count();
		session_ = std::move( next );
		++generation_;
		json( response, std::format( "{{\"generation\":{},\"open_ms\":{},\"planes\":", generation_, elapsed ) + planes +
			"],\"properties\":" + properties + "]}" );
		if (options_.log) options_.log( std::format( "Opened {} in {:.3f} ms", relative, elapsed ) );
	}

	void render ( const httplib::Request &request, httplib::Response &response ) {
		auto lock = std::unique_lock( mutex_, std::try_to_lock );
		if (!lock.owns_lock()) { fail( response, 409, "Renderer busy" ); return; }
		if (!session_) { fail( response, 409, "Open an image first" ); return; }
		if (integer( request, "generation", UINT64_MAX ) != generation_) {
			fail( response, 409, "Image changed; discard the old view request" ); return;
		}
		const auto plane_index = uint32_t( integer( request, "plane", session_->planes_.size() - 1 ) );
		const auto &plane = session_->planes_[plane_index];
		auto native = pyra_image_request{ sizeof( pyra_image_request ), plane_index, {}, 0 };
		if (request.get_param_value( "overview" ) == "1") {
			native.rectangle = plane.bounds;
			native.mpp = std::max( plane.bounds.width_um, plane.bounds.height_um ) / kOverviewEdge;
		} else {
			native.rectangle = { parameter( request, "x" ), parameter( request, "y" ),
				parameter( request, "width" ), parameter( request, "height" ) };
			native.mpp = parameter( request, "mpp" );
		}
		const auto &rect = native.rectangle;
		if (native.mpp <= 0 || rect.width_um <= 0 || rect.height_um <= 0 ||
			!std::isfinite( rect.x_um + rect.width_um ) || !std::isfinite( rect.y_um + rect.height_um ))
			throw std::invalid_argument( "Invalid view or raster exceeds the viewer limit" );
		auto layout = pyra_image_layout{};
		layout.struct_size = sizeof( layout );
		auto error = pyra_error{};
		if (pyra_get_image_layout( session_->wsi_, &native, &layout, &error ) != PYRA_OK)
			throw std::invalid_argument( error.message );
		if (layout.width > kMaxRenderEdge || layout.height > kMaxRenderEdge)
			throw std::invalid_argument( "Raster exceeds the viewer limit" );
		auto pixels = std::vector<unsigned char>( layout.capacity_bytes );
		const auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format,
			pixels.data(), pixels.size(), layout.row_stride_bytes };
		const auto start = Clock::now();
		const auto status = pyra_get_image( session_->wsi_, &native, &buffer, &error );
		const auto elapsed = std::chrono::duration<double, std::milli>( Clock::now() - start ).count();
		if (status != PYRA_OK && status != PYRA_INCOMPLETE) { fail( response, 400, error.message ); return; }
		response.set_header( "X-Pyra-Width", std::to_string( layout.width ) );
		response.set_header( "X-Pyra-Height", std::to_string( layout.height ) );
		response.set_header( "X-Pyra-Render-Ms", std::format( "{:.4f}", elapsed ) );
		response.set_header( "X-Pyra-Incomplete", status == PYRA_INCOMPLETE ? "1" : "0" );
		response.set_content( rgba_pixels( pixels, layout, plane.max_sample_value ), "application/octet-stream" );
		if (options_.log) options_.log( std::format( "Render plane={} {}x{} mpp={} native_ms={:.3f} incomplete={}",
			plane_index, layout.width, layout.height, native.mpp, elapsed, status == PYRA_INCOMPLETE ) );
	}

	ServerOptions options_;
	httplib::Server http_;
	std::string token_;
	int port_ = 0;
	std::mutex mutex_;
	std::unique_ptr<Session> session_;
	uint64_t generation_ = 0;

	struct Generation {
		bool running = false;
		uint64_t done = 0;
		uint64_t total = 0;
		std::string error;
		std::string path;		// relative to the root
	};
	std::mutex generate_mutex_;
	Generation synthetic_;
// Before generator_, after the state it reads: its destructor waits for a run.
	Analyser analyser_;
// Last: its destructor cancels and joins the writer before the state it uses goes.
	std::jthread generator_;
};


Server::Server ( ServerOptions options ) : impl_( std::make_unique<Impl>( std::move( options ) ) ) {}


Server::~Server () = default;


int Server::bind ( int port )
{
	impl_->port_ = port == 0 ? impl_->http_.bind_to_any_port( "127.0.0.1" ) :
		( impl_->http_.bind_to_port( "127.0.0.1", port ) ? port : -1 );
	return impl_->port_;
}


bool Server::listen () { return impl_->http_.listen_after_bind(); }


void Server::stop () { impl_->http_.stop(); }


bool Server::running () const { return impl_->http_.is_running(); }


const std::string &Server::token () const { return impl_->token_; }

} // namespace pyra::web
