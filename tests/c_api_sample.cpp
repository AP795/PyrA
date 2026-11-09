// Usage: pyra_c_api_sample input.svs [output.jpg]
#include <pyra/pyra.h>

#include <cstddef>
#include <cstdio>
#include <jpeglib.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr auto kImageEdge = uint32_t{ 1024 };
constexpr auto kJpegQuality = 95;

void check ( pyra_status status, const pyra_error &error )
{
	if (status != PYRA_OK)
		throw std::runtime_error( error.message );
}


std::string utf8 ( const std::filesystem::path &path )
{
	const auto value = path.u8string();
	return { value.begin(), value.end() };
}


void write_jpeg ( const std::filesystem::path &path, const pyra_image_layout &layout,
	const std::vector<uint8_t> &pixels );


int run ( const std::filesystem::path &input, const std::filesystem::path &output )
{
	pyra_context *context = nullptr;
	pyra_wsi *wsi = nullptr;
	auto error = pyra_error{};
	auto result = 1;
	try {
		auto path_error = std::error_code{};
		if (std::filesystem::equivalent( input, output, path_error ))
			throw std::runtime_error( "Input and output must be different files" );

// Open the WSI using the default native backend and context options.
		check( pyra_context_create( nullptr, &context, &error ), error );
		check( pyra_open( context, utf8( input ).c_str(), &wsi, &error ), error );
		auto plane = pyra_plane_info{};
		plane.struct_size = sizeof( plane );
		check( pyra_get_plane_info( wsi, 0, &plane, &error ), error );
		if (plane.pixel_format != PYRA_PIXEL_RGB8)
			throw std::runtime_error( "This getting-started example expects an RGB8 WSI" );

// A 1024-pixel square at native_mpp, centred on plane 0's metric bounds.
		auto request = pyra_image_request{};
		request.struct_size = sizeof( request );
		request.plane_index = 0;
		request.mpp = plane.native_mpp;
		const auto extent = kImageEdge * request.mpp;
		request.rectangle = { plane.bounds.x_um + ( plane.bounds.width_um - extent ) / 2,
			plane.bounds.y_um + ( plane.bounds.height_um - extent ) / 2, extent, extent };

// Ask Pyra for the buffer size, allocate it, then render synchronously.
		auto layout = pyra_image_layout{};
		layout.struct_size = sizeof( layout );
		check( pyra_get_image_layout( wsi, &request, &layout, &error ), error );
		auto pixels = std::vector<uint8_t>( layout.capacity_bytes );
		const auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout.pixel_format,
			pixels.data(), pixels.size(), layout.row_stride_bytes };
		check( pyra_get_image( wsi, &request, &buffer, &error ), error );
		write_jpeg( output, layout, pixels );
		result = 0;
	} catch (const std::exception &exception) {
		std::cerr << exception.what() << '\n';
	}

// Close the WSI before destroying its context, including on failure.
	if (wsi && pyra_close( wsi, &error ) != PYRA_OK) {
		std::cerr << error.message << '\n';
		result = 1;
	}
	if (context && pyra_context_destroy( context, &error ) != PYRA_OK) {
		std::cerr << error.message << '\n';
		result = 1;
	}
	if (result == 0)
		std::cout << "Saved " << utf8( output ) << " (" << kImageEdge << " x " << kImageEdge << ", native resolution, plane 0)\n";
	return result;
}


// JPEG output is independent of Pyra. The default libjpeg handler reports
// fatal codec errors and exits this command-line program.
void write_jpeg ( const std::filesystem::path &path, const pyra_image_layout &layout,
	const std::vector<uint8_t> &pixels )
{
	auto codec = jpeg_compress_struct{};
	auto error = jpeg_error_mgr{};
	codec.err = jpeg_std_error( &error );
	jpeg_create_compress( &codec );
	auto *encoded = static_cast<unsigned char *>( nullptr );
	auto size = 0ul;
	jpeg_mem_dest( &codec, &encoded, &size );
	codec.image_width = layout.width;
	codec.image_height = layout.height;
	codec.input_components = 3;
	codec.in_color_space = JCS_RGB;
	jpeg_set_defaults( &codec );
	jpeg_set_quality( &codec, kJpegQuality, TRUE );
	jpeg_start_compress( &codec, TRUE );
	while (codec.next_scanline < codec.image_height) {
		auto *row = const_cast<JSAMPROW>( pixels.data() + codec.next_scanline * layout.row_stride_bytes );
		if (jpeg_write_scanlines( &codec, &row, 1 ) != 1) {
			jpeg_destroy_compress( &codec );
			std::free( encoded );
			throw std::runtime_error( "JPEG encoder did not consume the scanline" );
		}
	}
	jpeg_finish_compress( &codec );
	jpeg_destroy_compress( &codec );
	const auto storage = std::unique_ptr<unsigned char, decltype( &std::free )>( encoded, std::free );
	auto output = std::ofstream( path, std::ios::binary );
	output.write( reinterpret_cast<const char *>( storage.get() ), std::streamsize( size ) );
	output.close();
	if (!output)
		throw std::runtime_error( "Cannot write JPEG: " + utf8( path ) );
}

}


#ifdef _WIN32
int wmain ( int argc, wchar_t **argv )
#else
int main ( int argc, char **argv )
#endif
{
	if (argc < 2 || argc > 3) {
		std::cerr << "Usage: pyra_c_api_sample input-wsi [output.jpg]\n";
		return 1;
	}
	return run( std::filesystem::path( argv[1] ),
		argc == 3 ? std::filesystem::path( argv[2] ) : std::filesystem::path( "centre.jpg" ) );
}
