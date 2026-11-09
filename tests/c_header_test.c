#include <pyra/pyra.h>

#include <stdio.h>

int main ( void )
{
	pyra_context_options options;
	pyra_context *context = NULL;
	pyra_error error;
	pyra_context_stats stats = { 0 };
	pyra_context_options_init( &options );
	if (options.abi_version != PYRA_ABI_VERSION ||
		pyra_context_create( &options, &context, &error ) != PYRA_OK) {
		fprintf( stderr, "Context creation: %s\n", error.message );
		return 1;
	}
	stats.struct_size = sizeof( stats );
	if (pyra_context_get_stats( context, &stats, &error ) != PYRA_OK ||
		stats.slide_handles != 0 || stats.shared_slides != 0)
		return 2;
	return pyra_context_destroy( context, &error ) == PYRA_OK ? 0 : 3;
}
