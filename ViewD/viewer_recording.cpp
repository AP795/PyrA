// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "viewer_recording.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <utility>

namespace pyra::test {
namespace {

constexpr auto kRecordingVersion = 1;

QJsonArray rectangle_json ( const QRectF &rect )
{
	return { rect.x(), rect.y(), rect.width(), rect.height() };
}


bool read_rectangle ( const QJsonValue &value, QRectF *rect )
{
	const auto values = value.toArray();
	if (values.size() != 4)
		return false;
	for (const auto &part : values)
		if (!part.isDouble() || !std::isfinite( part.toDouble() ))
			return false;
	*rect = QRectF( values[0].toDouble(), values[1].toDouble(), values[2].toDouble(), values[3].toDouble() );
	return rect->width() > 0 && rect->height() > 0 &&
		std::isfinite( rect->right() ) && std::isfinite( rect->bottom() );
}


bool read_plane ( const QJsonObject &object, PlaneSelection *plane )
{
	if (!object["channel"].isString() || !object["z"].isDouble() ||
		!std::isfinite( object["z"].toDouble() ))
		return false;
	plane->channel = object["channel"].toString();
	plane->zlevel = object["z"].toDouble();
	return true;
}

} // namespace


bool valid_render_request ( const RenderRequest &request )
{
	return std::isfinite( request.mpp ) && request.mpp > 0 &&
		std::isfinite( request.plane.zlevel ) &&
		std::isfinite( request.xywh.x() ) && std::isfinite( request.xywh.y() ) &&
		std::isfinite( request.xywh.width() ) && std::isfinite( request.xywh.height() ) &&
		std::isfinite( request.xywh.right() ) && std::isfinite( request.xywh.bottom() ) &&
		request.xywh.width() > 0 && request.xywh.height() > 0;
}


QByteArray encode_recording ( const ViewerRecording &recording )
{
	auto planes = QJsonArray{};
	for (const auto &plane : recording.planes)
		planes.append( QJsonObject{ { "channel", plane.plane.channel }, { "z", plane.plane.zlevel },
			{ "bounds", rectangle_json( plane.bounds ) }, { "mpp", double( plane.native_mpp ) },
			{ "format", int( plane.pixel_format ) }, { "z-is-index", plane.z_is_index } } );
	auto requests = QJsonArray{};
	for (const auto &request : recording.requests)
		requests.append( QJsonObject{ { "channel", request.plane.channel }, { "z", request.plane.zlevel },
			{ "rect", rectangle_json( request.xywh ) }, { "mpp", request.mpp } } );
	return QJsonDocument( QJsonObject{ { "version", kRecordingVersion }, { "source", recording.source },
		{ "size", QString::number( recording.source_size ) },
		{ "modified-ms", QString::number( recording.source_modified_ms ) },
		{ "planes", planes }, { "requests", requests } } ).toJson( QJsonDocument::Compact );
}


bool decode_recording ( const QByteArray &data, ViewerRecording *recording, QString *error )
{
	const auto fail = [error] {
		if (error)
			*error = QStringLiteral( "Invalid saved recording data or version." );
		return false;
	};
	if (!recording)
		return fail();
	const auto document = QJsonDocument::fromJson( data );
	if (!document.isObject())
		return fail();
	const auto object = document.object();
	auto result = ViewerRecording{};
	result.source = object["source"].toString();
	auto size_ok = false;
	auto time_ok = false;
	result.source_size = object["size"].toString().toLongLong( &size_ok );
	result.source_modified_ms = object["modified-ms"].toString().toLongLong( &time_ok );
	if (object["version"].toInt( -1 ) != kRecordingVersion || result.source.isEmpty() ||
		!size_ok || !time_ok || result.source_size < 0 || !object["planes"].isArray() || !object["requests"].isArray())
		return fail();
	for (const auto &value : object["planes"].toArray()) {
		const auto item = value.toObject();
		auto plane = PlaneInfo{};
		plane.native_mpp = float( item["mpp"].toDouble() );
		const auto format = item["format"].toInt();
		if (!read_plane( item, &plane.plane ) || !read_rectangle( item["bounds"], &plane.bounds ) ||
			!std::isfinite( plane.native_mpp ) || plane.native_mpp <= 0 ||
			format < PYRA_PIXEL_RGB8 || format > PYRA_PIXEL_FLOAT32)
			return fail();
		plane.pixel_format = uint32_t( format );
		plane.z_is_index = item["z-is-index"].toBool();
		result.planes.push_back( plane );
	}
	for (const auto &value : object["requests"].toArray()) {
		const auto item = value.toObject();
		auto request = RenderRequest{};
		request.mpp = item["mpp"].toDouble();
		if (!read_plane( item, &request.plane ) || !read_rectangle( item["rect"], &request.xywh ) ||
			!valid_render_request( request ))
			return fail();
		result.requests.push_back( request );
	}
	if (!validate_recording( result, result ).isEmpty())
		return fail();
	*recording = std::move( result );
	if (error)
		error->clear();
	return true;
}


QString validate_recording ( const ViewerRecording &recording, const ViewerRecording &current )
{
	if (recording.source != current.source || recording.source_size != current.source_size ||
		recording.source_modified_ms != current.source_modified_ms)
		return QStringLiteral( "The saved image path, size or modification time differs from the open image." );
	const auto near = [] ( double a, double b ) {
		return std::isfinite( a ) && std::isfinite( b ) &&
			std::abs( a - b ) <= 1e-5 * std::max( { 1.0, std::abs( a ), std::abs( b ) } );
	};
	for (const auto &request : recording.requests) {
		if (!valid_render_request( request ))
			return QStringLiteral( "A saved render rectangle, MPP or Z value is invalid." );
		const auto find = [&request] ( const auto &planes ) {
			return std::find_if( planes.begin(), planes.end(), [&request] ( const auto &plane ) {
				return plane.plane == request.plane;
			} );
		};
		const auto before = find( recording.planes );
		const auto after = find( current.planes );
		if (before == recording.planes.end() || after == current.planes.end() ||
			before->pixel_format != after->pixel_format || before->z_is_index != after->z_is_index ||
			!near( before->native_mpp, after->native_mpp ) ||
			!near( before->bounds.x(), after->bounds.x() ) || !near( before->bounds.y(), after->bounds.y() ) ||
			!near( before->bounds.width(), after->bounds.width() ) || !near( before->bounds.height(), after->bounds.height() ))
			return QStringLiteral( "A recorded plane is missing or its pixel format or metric geometry has changed." );
	}
	return {};
}

} // namespace pyra::test
