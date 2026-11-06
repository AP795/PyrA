#pragma once

#include "wsi_session.h"

#include "pyr_rt.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace priml {


class WsiSessionRegistry {
public:
	static WsiSessionRegistry &instance ()
	{
		static auto registry = WsiSessionRegistry{};
		return registry;
	}

	[[nodiscard]] SharedWsiSession acquire ( const std::filesystem::path &source_path,
		const WsiSessionConfig &config = {}, bool *opened_now = nullptr )
	{
		if (opened_now)
			*opened_now = false;

		auto canonical_path = canonicalize_path_local( source_path );
		if (canonical_path.empty())
			return {};

		auto key = canonical_path.native();
		auto session = SharedWsiSession{};
		{
			auto guard = std::scoped_lock( mutex_ );
			if (auto it = sessions_.find( key ); it != sessions_.end())
				session = it->second.lock();
			if (!session) {
				session = std::make_shared<WsiSession>();
				sessions_[key] = session;
			}
		}

		if (!session->ensure_open( canonical_path, config, opened_now )) {
			auto guard = std::scoped_lock( mutex_ );
			if (auto it = sessions_.find( key ); it != sessions_.end()) {
				auto live = it->second.lock();
				if (!live || live == session)
					sessions_.erase( it );
			}
			return {};
		}

		return session;
	}

private:
	using SessionKey = std::filesystem::path::string_type;

	static std::filesystem::path canonicalize_path_local (
		const std::filesystem::path &source_path )
	{
		if (source_path.empty())
			return {};

		auto ec = std::error_code{};
		auto absolute = std::filesystem::absolute( source_path, ec );
		if (ec)
			return pyr::normalise_path( source_path );

		auto canonical = std::filesystem::weakly_canonical( absolute, ec );
		if (!ec)
			return pyr::normalise_path( canonical );

		return pyr::normalise_path( absolute );
	}

	std::mutex mutex_;
	std::unordered_map<SessionKey, std::weak_ptr<WsiSession>> sessions_;
};

} // namespace priml
