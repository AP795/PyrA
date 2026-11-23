// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "reader_options.h"

#include <QByteArray>
#include <QRectF>
#include <QString>

#include <vector>

namespace pyra::test {

struct PlaneSelection {
	QString channel;
	double zlevel = 0;

	bool operator== ( const PlaneSelection & ) const = default;
};

struct RenderRequest {
	QRectF xywh;
	double mpp = 1.0;
	PlaneSelection plane;

	bool operator== ( const RenderRequest & ) const = default;
};

struct PlaneInfo {
	PlaneSelection plane;
	QRectF bounds;
	float native_mpp = 1.0f;
	QString label;
	uint32_t index = 0;
	pyra_pixel_format pixel_format = 0;
	double max_sample_value = 1;
	bool z_is_index = false;
};

struct ViewerRecording {
	QString source;
	qint64 source_size = -1;
	qint64 source_modified_ms = -1;
	std::vector<PlaneInfo> planes;
	std::vector<RenderRequest> requests;
};

bool valid_render_request ( const RenderRequest &request );
QByteArray encode_recording ( const ViewerRecording &recording );
bool decode_recording ( const QByteArray &data, ViewerRecording *recording, QString *error );
// Empty on success. The current open image remains usable on validation failure.
QString validate_recording ( const ViewerRecording &recording, const ViewerRecording &current );

} // namespace pyra::test
