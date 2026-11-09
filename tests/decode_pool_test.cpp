#include "wsi.h"

#include <atomic>
#include <iostream>
#include <stdexcept>

int main ()
{
	auto pool = priml::DecodePool{};
	try {
		for (const auto failing_worker : { 1u, 0u }) {
			auto calls = std::atomic<unsigned>{ 0 };
			auto caught = false;
			try {
				pool.run_batch( 3, [&] ( unsigned worker ) {
					++calls;
					if (worker == failing_worker)
						throw std::runtime_error( "Expected decode worker failure" );
				} );
			} catch (const std::runtime_error &) {
				caught = true;
			}
			if (!caught || calls != 3)
				throw std::runtime_error( "Batch did not join all workers and propagate its exception" );
		}
		auto calls = std::atomic<unsigned>{ 0 };
		pool.run_batch( 4, [&] ( unsigned ) { ++calls; } );
		if (calls != 4)
			throw std::runtime_error( "Pool was not reusable after a failed batch" );
		pool.shutdown();
		pool.run_batch( 2, [&] ( unsigned ) { ++calls; } );
		if (calls != 6)
			throw std::runtime_error( "Pool could not restart after shutdown" );
		std::cout << "Worker/caller exceptions, reuse, growth, and restart: PASS\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
