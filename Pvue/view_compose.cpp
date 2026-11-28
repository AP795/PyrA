// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "view_compose.h"
#include "display_rgba.h"

#include "pyr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace pyra::pvue {

namespace {

bool is_gray ( pyra_pixel_format format )
{
	return format == PYRA_PIXEL_GRAY8 || format == PYRA_PIXEL_GRAY16;
}


// The same focal position: equal, or both unknown (index-only stacks).
bool same_z ( double left, double right )
{
	return left == right || ( std::isnan( left ) && std::isnan( right ) );
}


pyra_status fail ( pyra_status status, const pyra_error &failure, std::string *error )
{
	if (error)
		*error = failure.message;
	return status;
}


bool failed ( const pyra_error &failure, std::string *error )
{
	if (error)
		*error = failure.message;
	return false;
}


// Renders `plane` into `samples`, laid out as `layout`.
pyra_status render_samples ( pyra_wsi *wsi, uint32_t plane, const ViewRect &view, std::vector<unsigned char> *samples,
	pyra_image_layout *layout, std::string *error )
{
	auto failure = pyra_error{};
	const auto request = pyra_image_request{ sizeof( pyra_image_request ), plane,
		{ view.x_um, view.y_um, view.width_um, view.height_um }, view.mpp };
	*layout = pyra_image_layout{};
	layout->struct_size = sizeof( *layout );
	auto status = pyra_get_image_layout( wsi, &request, layout, &failure );
	if (status != PYRA_OK)
		return fail( status, failure, error );
//	Reused, not cleared: pyra_get_image writes every pixel, missing areas included.
	if (samples->size() < layout->capacity_bytes)
		samples->resize( layout->capacity_bytes );
	const auto buffer = pyra_image_buffer{ sizeof( pyra_image_buffer ), layout->pixel_format,
		samples->data(), samples->size(), layout->row_stride_bytes };
	status = pyra_get_image( wsi, &request, &buffer, &failure );
	if (status != PYRA_OK && status != PYRA_INCOMPLETE)
		return fail( status, failure, error );
	return status;
}

} // namespace


DisplayRecipe display_recipe ( const std::vector<PlaneDesc> &planes, bool gallery, uint32_t reference )
{
	if (reference >= planes.size())
		return {};
	const auto &chosen = planes[reference];
	if (gallery)
		return { DisplayKind::Gallery, { reference } };
	if (!chosen.channel.empty() && is_gray( chosen.format )) {
		uint32_t slots[3] = {};
		bool found[3] = {};
		for (auto i = uint32_t{ 0 }; i < planes.size(); ++i) {
			const auto &plane = planes[i];
			if (plane.channel.empty() || !is_gray( plane.format ) || !same_z( plane.z, chosen.z ))
				continue;
			if (const auto slot = priml::rgb_channel_index( plane.channel ); slot >= 0 && !found[slot]) {
				found[slot] = true;
				slots[slot] = i;
			}
		}
		if (found[0] && found[1] && found[2])
			return { DisplayKind::Channels, { slots[0], slots[1], slots[2] } };
		return { DisplayKind::Gray, { reference } };
	}
	switch (chosen.format) {
		case PYRA_PIXEL_RGB8: return { DisplayKind::Rgb8, { reference } };
		case PYRA_PIXEL_RGB16: return { DisplayKind::Rgb16, { reference } };
		case PYRA_PIXEL_GRAY8:
		case PYRA_PIXEL_GRAY16: return { DisplayKind::Gray, { reference } };
		default: return { DisplayKind::None, { reference } };
	}
}


bool slide_planes ( pyra_wsi *wsi, std::vector<PlaneDesc> *planes, bool *gallery, std::string *error )
{
	planes->clear();
	*gallery = false;
	auto failure = pyra_error{};
	auto count = uint32_t{};
	if (pyra_get_plane_count( wsi, &count, &failure ) != PYRA_OK)
		return failed( failure, error );
	for (auto i = 0u; i < count; ++i) {
		auto info = pyra_plane_info{};
		info.struct_size = sizeof( info );
		if (pyra_get_plane_info( wsi, i, &info, &failure ) != PYRA_OK)
			return failed( failure, error );
		planes->push_back( { info.channel ? info.channel : "", info.pixel_format, info.max_sample_value, info.z_um } );
	}
	if (pyra_get_property_count( wsi, &count, &failure ) != PYRA_OK)
		return failed( failure, error );
	for (auto i = 0u; i < count && !*gallery; ++i) {
		auto property = pyra_property{};
		if (pyra_get_property( wsi, i, &property, &failure ) != PYRA_OK)
			return failed( failure, error );
		*gallery = property.key && kGalleryProperty == property.key;
	}
	return true;
}


pyra_status render_plane ( pyra_wsi *wsi, uint32_t plane, const ViewRect &view, Frame *frame, std::string *error )
{
	auto failure = pyra_error{};
	auto info = pyra_plane_info{};
	info.struct_size = sizeof( info );
	if (const auto status = pyra_get_plane_info( wsi, plane, &info, &failure ); status != PYRA_OK)
		return fail( status, failure, error );
	auto layout = pyra_image_layout{};
	const auto status = render_samples( wsi, plane, view, &frame->samples, &layout, error );
	if (status != PYRA_OK && status != PYRA_INCOMPLETE)
		return status;
	frame->width = layout.width;
	frame->height = layout.height;
	frame->rgba.resize( size_t( layout.width ) * layout.height * 4 );
	display_rgba( frame->samples.data(), layout, info.max_sample_value, frame->rgba.data() );
	return status;
}


pyra_status render_display ( pyra_wsi *wsi, const DisplayRecipe &recipe, const ViewRect &view, Frame *frame,
	std::string *error )
{
	if (recipe.planes.empty()) {
		if (error) *error = "The display recipe names no plane";
		return PYRA_INVALID_ARGUMENT;
	}
	if (!recipe.composite())
		return render_plane( wsi, recipe.planes.front(), view, frame, error );
	auto rasters = std::array<ChannelRaster, 3>{};
	auto result = PYRA_OK;
	for (auto i = 0; i < 3; ++i) {
		auto failure = pyra_error{};
		auto info = pyra_plane_info{};
		info.struct_size = sizeof( info );
		if (const auto status = pyra_get_plane_info( wsi, recipe.planes[i], &info, &failure ); status != PYRA_OK)
			return fail( status, failure, error );
		auto layout = pyra_image_layout{};
		const auto status = render_samples( wsi, recipe.planes[i], view, &frame->channel_samples[i], &layout, error );
		if (status != PYRA_OK && status != PYRA_INCOMPLETE)
			return status;
		if (status == PYRA_INCOMPLETE)
			result = PYRA_INCOMPLETE;
		rasters[i] = { frame->channel_samples[i].data(), layout.row_stride_bytes, layout.width, layout.height,
			layout.pixel_format, info.max_sample_value };
	}
	if (!compose_display( rasters, &frame->rgba, &frame->width, &frame->height )) {
		if (error) *error = "The R, G and B channels do not render to one raster";
		return PYRA_UNSUPPORTED;
	}
	return result;
}


bool compose_display ( const std::array<ChannelRaster, 3> &channels, std::vector<unsigned char> *rgba, uint32_t *width,
	uint32_t *height )
{
	const auto w = channels[0].width;
	const auto h = channels[0].height;
	if (w == 0 || h == 0)
		return false;
	auto pixels = std::array<priml::Pwhl<uint16_t>, 3>{};
	auto named = std::vector<priml::NamedChannelPixels>{};
	static constexpr const char *kNames[3] = { "R", "G", "B" };
	for (auto c = 0; c < 3; ++c) {
		const auto &channel = channels[c];
		if (!is_gray( channel.format ) || channel.width != w || channel.height != h || !channel.data)
			return false;
		pixels[c] = priml::Pwhl<uint16_t>( priml::Dim2D<int>{ int( w ), int( h ) }, priml::uninit );
		for (auto y = 0u; y < h; ++y) {
			const auto *row = channel.data + size_t( y ) * channel.row_stride_bytes;
			auto *out = pixels[c].rowp( int( y ) );
			if (channel.format == PYRA_PIXEL_GRAY16)
				std::memcpy( out, row, size_t( w ) * sizeof( uint16_t ) );
			else
				std::copy_n( row, w, out );
		}
		named.push_back( { kNames[c], &pixels[c], int( std::lround( channel.range ) ) } );
	}
	const auto composed = priml::compose_rgb_from_channels( named );
	if (!composed.is_set())
		return false;
	*width = w;
	*height = h;
	rgba->resize( size_t( w ) * h * 4 );
	for (auto y = 0u; y < h; ++y) {
		const auto *row = composed.rowp( int( y ) );
		auto *out = rgba->data() + size_t( y ) * w * 4;
		for (auto x = 0u; x < w; ++x, out += 4) {
			out[0] = row[x].r;
			out[1] = row[x].g;
			out[2] = row[x].b;
			out[3] = 255;
		}
	}
	return true;
}

} // namespace pyra::pvue
