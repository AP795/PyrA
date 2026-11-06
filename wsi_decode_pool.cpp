#include "wsi.h"

namespace priml {

// --- DecodePool ---

DecodePool::~DecodePool () {
	shutdown();
}


void DecodePool::shutdown () {
	auto batch_guard = std::unique_lock( batch_mtx_ );
	{
		std::lock_guard lk( mtx_ );
		if (stop_) return;
		stop_ = true;
	}
	cv_start_.notify_all();
	for (auto &t : threads_)
		if (t.joinable()) t.join();
	threads_.clear();
	{
		auto guard = std::scoped_lock( file_handles_mtx_ );
		file_handles_.clear();
	}
	work_fn_ = nullptr;
	generation_ = 0;
	batch_size_ = 0;
	done_ = 0;
	stop_ = false;
}


void DecodePool::ensure_workers ( unsigned count ) {
	while (threads_.size() < count) {
		auto idx = (unsigned)threads_.size();
		threads_.emplace_back( [this, idx] { worker_loop( idx ); } );
	}
//
// Handle rows are grown lazily per file by worker_file, under its own lock, so
// there is nothing to pre-size here: a slide whose levels all live in one file
// only ever populates the default row.
}


void DecodePool::worker_loop ( unsigned pool_idx ) {
	unsigned last_gen = 0;
	while (true) {
		{
			std::unique_lock lk( mtx_ );
			cv_start_.wait( lk, [&] {
				return stop_ || (generation_ > last_gen && pool_idx < batch_size_);
			} );
			if (stop_) return;
			last_gen = generation_;
		}
		(*work_fn_)( pool_idx + 1 );
		{
			std::lock_guard lk( mtx_ );
			++done_;
		}
		cv_done_.notify_one();
	}
}


void DecodePool::run_batch ( unsigned worker_count, std::function<void(unsigned)> fn ) {
	auto batch_guard = std::unique_lock( batch_mtx_ );
	(void)batch_guard;
	if (worker_count == 0) return;
	if (worker_count == 1) {
		fn( 0 );
		return;
	}
	auto pool_count = worker_count - 1;
	{
		std::lock_guard lk( mtx_ );
		work_fn_ = &fn;
		batch_size_ = pool_count;
		done_ = 0;
		++generation_;
	}
	ensure_workers( pool_count );
	cv_start_.notify_all();
	fn( 0 );
	{
		std::unique_lock lk( mtx_ );
		cv_done_.wait( lk, [&] { return done_ >= pool_count; } );
		work_fn_ = nullptr;
	}
}


void DecodePool::set_file ( const std::string &filename ) {
	auto guard = std::scoped_lock( file_handles_mtx_ );
	filename_ = filename;
	file_handles_.clear();
}


std::ifstream &DecodePool::worker_file ( unsigned pool_idx,
	const std::string &path )
{
	auto guard = std::scoped_lock( file_handles_mtx_ );
	auto &row = file_handles_[path];
	if (pool_idx >= row.size())
		row.resize( pool_idx + 1 );
	auto &slot = row[pool_idx];
	if (!slot)
		slot = std::make_unique<std::ifstream>();
	const auto &name = path.empty() ? filename_ : path;
	if (!slot->is_open() && !name.empty())
		slot->open( name, std::ios::binary );
	return *slot;
}


} // namespace priml
