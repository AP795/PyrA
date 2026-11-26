// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include <pyra/pyra.h>

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

namespace pyra::test {

// Parameters of a synthetic z-stack (pyra_make_synthetic) and where to write it.
// Tile size 0 writes an untiled TIFF: one plane, LZW or JPEG.
class GenerateWsiDialog : public QDialog {
public:
	GenerateWsiDialog ( const QString &directory, QWidget *parent = nullptr );
	QString path () const;
	pyra_synthetic_options options () const;
	bool untiled () const;

private:
	void tile_changed ( int value );
	void update_layout ();
	void update_codecs ();
	void update_quality ();
	int last_tile_ = 0;
	QLineEdit *path_ = nullptr;
	QSpinBox *width_ = nullptr;
	QSpinBox *height_ = nullptr;
	QSpinBox *tile_ = nullptr;
	QSpinBox *z_levels_ = nullptr;
	QDoubleSpinBox *mpp_ = nullptr;
	QDoubleSpinBox *z_step_ = nullptr;
	QDoubleSpinBox *focus_ = nullptr;
	QDoubleSpinBox *nucleus_ = nullptr;
	QDoubleSpinBox *spacing_ = nullptr;
	QDoubleSpinBox *positive_ = nullptr;
	QDoubleSpinBox *tumour_ = nullptr;
	QDoubleSpinBox *lymphocytes_ = nullptr;
	QComboBox *codec_ = nullptr;
	QSpinBox *quality_ = nullptr;
	QCheckBox *lossless_ = nullptr;
};

} // namespace pyra::test
