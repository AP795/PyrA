// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include "render_evaluator.h"

#include <QWidget>

class QLabel;
class QProgressBar;

namespace pyra::test {

class CacheUsageWidget : public QWidget {
	Q_OBJECT
public:
	explicit CacheUsageWidget ( QWidget *parent = nullptr );
	void set_usage ( const CacheUsage &usage );

private:
	QLabel *tbf_text_ = nullptr;
	QLabel *pbf_text_ = nullptr;
	QProgressBar *tbf_bar_ = nullptr;
	QProgressBar *pbf_bar_ = nullptr;
};

} // namespace pyra::test
