#pragma once

#include "wsi.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>

namespace priml {


struct WsiSessionConfig {
	int decode_threads = 0;
	bool use_pixbf = true;
// pixbf_mode=auto: the loader picks, once it has seen what the file stores.
	bool use_pixbf_auto = false;
	size_t tilebf_init_bytes = 0;
	size_t tilebf_cap_bytes = 0;
	size_t pixbf_init_bytes = 0;
	size_t pixbf_cap_bytes = 0;
	size_t ram_cap_bytes = 0;
};


class WsiSession {
public:
	WsiSession ()
		: config_{
			pyramid_.decode_threads,
			pyramid_.use_pixbf,
			pyramid_.use_pixbf_auto,
			pyramid_.tbf_init_bytes,
			pyramid_.tbf_cap_bytes,
			pyramid_.pxb_init_bytes,
			pyramid_.pxb_cap_bytes,
			pyramid_.ram_cap_bytes }
	{
	}

	WsiSession ( const WsiSession & ) = delete;
	WsiSession &operator= ( const WsiSession & ) = delete;

	~WsiSession ()
	{
		close();
	}

	[[nodiscard]] bool open ( const std::filesystem::path &source_path )
	{
		auto guard = std::scoped_lock( lifecycle_mutex_ );
		return open_locked( source_path );
	}

	[[nodiscard]] bool ensure_open ( const std::filesystem::path &source_path,
		const WsiSessionConfig &config, bool *opened_now = nullptr )
	{
		auto guard = std::scoped_lock( lifecycle_mutex_ );
		if (opened_now)
			*opened_now = false;

		apply_config_locked( config );
		if (opened_ && loader_ && source_path_ == source_path)
			return true;

		auto ok = open_locked( source_path );
		if (opened_now && ok)
			*opened_now = true;
		return ok;
	}

	void close ()
	{
		auto guard = std::scoped_lock( lifecycle_mutex_ );
		close_locked();
	}

	[[nodiscard]] bool is_open () const
	{
		return opened_ && loader_ != nullptr && !source_path_.empty();
	}

	[[nodiscard]] const std::filesystem::path &source_path () const
	{
		return source_path_;
	}

	[[nodiscard]] float base_mpp () const
	{
		return base_mpp_;
	}

	[[nodiscard]] const WsiNativeGeometry &native_geometry () const
	{
		return native_geometry_;
	}

	[[nodiscard]] std::optional<WsiNativeGeometry> native_geometry (
		const PlaneSel &plane ) const
	{
		return native_geometry_for_plane( pyramid_, plane );
	}

	[[nodiscard]] WsiLoader *loader () const
	{
		return loader_.get();
	}

	[[nodiscard]] Ppyr &pyramid ()
	{
		return pyramid_;
	}

	[[nodiscard]] const Ppyr &pyramid () const
	{
		return pyramid_;
	}

	[[nodiscard]] WsiSessionConfig config () const
	{
		auto guard = std::scoped_lock( lifecycle_mutex_ );
		return config_;
	}

	void set_decode_threads ( int decode_threads )
	{
		auto guard = std::scoped_lock( lifecycle_mutex_ );
		config_.decode_threads = std::max( 0, decode_threads );
		apply_runtime_settings_locked();
	}

	void set_use_pixbf ( bool use_pixbf, bool automatic = false )
	{
		auto guard = std::scoped_lock( lifecycle_mutex_ );
		config_.use_pixbf = use_pixbf;
		config_.use_pixbf_auto = automatic;
		apply_runtime_settings_locked();
	}

	void set_buffer_budgets ( size_t tilebf_init_bytes, size_t tilebf_cap_bytes,
		size_t pixbf_init_bytes, size_t pixbf_cap_bytes, size_t ram_cap_bytes )
	{
		auto guard = std::scoped_lock( lifecycle_mutex_ );
		config_.tilebf_init_bytes = tilebf_init_bytes;
		config_.tilebf_cap_bytes = tilebf_cap_bytes;
		config_.pixbf_init_bytes = pixbf_init_bytes;
		config_.pixbf_cap_bytes = pixbf_cap_bytes;
		config_.ram_cap_bytes = ram_cap_bytes;
		apply_runtime_settings_locked();
	}

private:
	void apply_config_locked ( const WsiSessionConfig &config )
	{
		config_ = config;
		config_.decode_threads = std::max( 0, config_.decode_threads );
		apply_runtime_settings_locked();
	}

	void apply_runtime_settings_locked ()
	{
		pyramid_.decode_threads = config_.decode_threads;
//
// Only meaningful before OpenFile: once a loader has resolved auto, overwriting
// use_pixbf here would undo its choice, so leave it alone in that case.
		pyramid_.use_pixbf_auto = config_.use_pixbf_auto;
		if (!config_.use_pixbf_auto || !is_open())
			pyramid_.use_pixbf = config_.use_pixbf;
		pyramid_.tbf_init_bytes = config_.tilebf_init_bytes;
		pyramid_.tbf_cap_bytes = config_.tilebf_cap_bytes;
		pyramid_.pxb_init_bytes = config_.pixbf_init_bytes;
		pyramid_.pxb_cap_bytes = config_.pixbf_cap_bytes;
		pyramid_.ram_cap_bytes = config_.ram_cap_bytes;
	}

	void close_locked ()
	{
		if (loader_)
			loader_->Close( pyramid_ );
		loader_.reset();
		pyramid_ = Ppyr{};
		apply_runtime_settings_locked();
		source_path_.clear();
		base_mpp_ = 0.0f;
		native_geometry_ = {};
		opened_ = false;
	}

	[[nodiscard]] bool open_locked ( const std::filesystem::path &source_path )
	{
		close_locked();
		source_path_ = source_path;
		if (source_path_.empty())
			return false;

		loader_ = WsiLoader::Create( source_path_.string() );
		if (!loader_)
			return false;
//
// Opened as the file says its levels decode to, not as Rgb regardless.
//
// One open carries one pixel type, so opening a channel slide as Rgb drops every
// plane it has and the session comes back empty - after which each zone reopens
// the file for itself and the shared session buys nothing. A file that declares
// nothing is Rgb8, which is every format but PTF and so every slide that worked
// before this.
		auto declared = LevelPixelType::Rgb8;
		ProbeWsiPixelType( source_path_.string(), &declared );
		const auto opened = visit_level_pixel_type( declared, [this] ( auto pixel ) {
			return loader_->OpenFile<decltype( pixel )>( source_path_.string(), pyramid_ );
		} );
		if (!opened) {
			loader_.reset();
			return false;
		}

		opened_ = true;
		apply_runtime_settings_locked();
		if (auto geometry = display_native_geometry( pyramid_ )) {
			native_geometry_ = *geometry;
			base_mpp_ = geometry->render_mpp;
		}
		return true;
	}

	std::filesystem::path source_path_;
	std::unique_ptr<WsiLoader> loader_;
	Ppyr pyramid_;
	float base_mpp_ = 0.0f;
	WsiNativeGeometry native_geometry_;
	WsiSessionConfig config_;
	bool opened_ = false;
	mutable std::mutex lifecycle_mutex_;
};

using SharedWsiSession = std::shared_ptr<WsiSession>;

} // namespace priml
