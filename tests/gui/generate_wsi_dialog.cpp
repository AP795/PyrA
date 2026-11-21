// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "generate_wsi_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace pyra::test {

namespace {

const auto kDefaultFileName = QStringLiteral( "synthetic_cells.ptf" );
// Fractions are edited as percentages.
constexpr double kPercent = 100.0;

struct CodecChoice {
	pyra_codec value;
	const char *label;
};

constexpr CodecChoice kCodecs[] = {
	{ PYRA_CODEC_JPEG, "JPEG" },
	{ PYRA_CODEC_JPEG2000, "JPEG 2000" },
	{ PYRA_CODEC_HTJ2K, "HTJ2K" },
	{ PYRA_CODEC_JPEGXL, "JPEG XL" },
};


QSpinBox *make_spin ( QWidget *parent, const QString &name, int minimum, int maximum, int value )
{
	auto *spin = new QSpinBox( parent );
	spin->setObjectName( name );
	spin->setRange( minimum, maximum );
	spin->setValue( value );
	return spin;
}


QDoubleSpinBox *make_real ( QWidget *parent, const QString &name, double minimum, double maximum,
	double value, int decimals, const QString &suffix )
{
	auto *spin = new QDoubleSpinBox( parent );
	spin->setObjectName( name );
	spin->setDecimals( decimals );
	spin->setRange( minimum, maximum );
	spin->setValue( value );
	spin->setSuffix( suffix );
	return spin;
}


QDoubleSpinBox *make_percent ( QWidget *parent, const QString &name, double fraction )
{
	return make_real( parent, name, 0.0, kPercent, fraction * kPercent, 1, QStringLiteral( " %" ) );
}

} // namespace


GenerateWsiDialog::GenerateWsiDialog ( const QString &directory, QWidget *parent )
	: QDialog( parent )
{
	setWindowTitle( tr( "Generate synthetic WSI" ) );
	auto defaults = pyra_synthetic_options{};
	pyra_synthetic_options_init( &defaults );
	auto *layout = new QVBoxLayout( this );
	auto *form = new QFormLayout;
	auto *slide_box = new QGroupBox( tr( "Slide" ), this );
	auto *slide_form = new QFormLayout( slide_box );
	auto *focus_box = new QGroupBox( tr( "Focus" ), this );
	auto *focus_form = new QFormLayout( focus_box );
	auto *tissue_box = new QGroupBox( tr( "Tissue" ), this );
	auto *tissue_form = new QFormLayout( tissue_box );
	auto *file_row = new QHBoxLayout;
	path_ = new QLineEdit( QDir( directory ).filePath( kDefaultFileName ), this );
	path_->setObjectName( QStringLiteral( "synthPath" ) );
	path_->setAccessibleName( tr( "Output PTF file" ) );
	auto *browse = new QPushButton( tr( "Browse…" ), this );
	browse->setAutoDefault( false );
	file_row->addWidget( path_, 1 );
	file_row->addWidget( browse );
	form->addRow( tr( "Output file:" ), file_row );

	const auto edge_max = int( PYRA_SYNTHETIC_EDGE_MAX );
	width_ = make_spin( this, QStringLiteral( "synthWidth" ), 1, edge_max, int( defaults.width ) );
	height_ = make_spin( this, QStringLiteral( "synthHeight" ), 1, edge_max, int( defaults.height ) );
	width_->setSuffix( tr( " px" ) );
	height_->setSuffix( tr( " px" ) );
	auto *size_row = new QHBoxLayout;
	size_row->addWidget( width_, 1 );
	size_row->addWidget( new QLabel( QStringLiteral( "×" ), this ) );
	size_row->addWidget( height_, 1 );
	slide_form->addRow( tr( "&Image size:" ), size_row );
	tile_ = make_spin( this, QStringLiteral( "synthTile" ), int( PYRA_SYNTHETIC_TILE_MIN ),
		int( PYRA_SYNTHETIC_TILE_MAX ), int( defaults.tile_edge ) );
	tile_->setSingleStep( int( PYRA_SYNTHETIC_TILE_STEP ) );
	tile_->setSuffix( tr( " px" ) );
	tile_->setToolTip( tr( "Square tile edge, a multiple of %1" ).arg( PYRA_SYNTHETIC_TILE_STEP ) );
	slide_form->addRow( tr( "&Tile size:" ), tile_ );
	mpp_ = make_real( this, QStringLiteral( "synthMpp" ), PYRA_SYNTHETIC_MPP_MIN, PYRA_SYNTHETIC_MPP_MAX,
		defaults.mpp, 3, tr( " µm/px" ) );
	mpp_->setSingleStep( 0.05 );
	slide_form->addRow( tr( "&Pixel size:" ), mpp_ );

	z_levels_ = make_spin( this, QStringLiteral( "synthZ" ), 1, int( PYRA_SYNTHETIC_Z_MAX ),
		int( defaults.z_levels ) );
	focus_form->addRow( tr( "&Z levels:" ), z_levels_ );
	z_step_ = make_real( this, QStringLiteral( "synthZStep" ), PYRA_SYNTHETIC_Z_STEP_MIN_UM,
		PYRA_SYNTHETIC_Z_STEP_MAX_UM, defaults.z_step_um, 2, tr( " µm" ) );
	z_step_->setSingleStep( 0.25 );
	focus_form->addRow( tr( "Plane &step:" ), z_step_ );
	focus_ = make_percent( this, QStringLiteral( "synthFocus" ), defaults.focus_fraction );
	focus_->setDecimals( 2 );
	focus_->setValue( defaults.focus_fraction * kPercent );
	focus_->setToolTip( tr( "In-focus depth: 0 % = first plane, 100 % = last plane" ) );
	focus_form->addRow( tr( "&Focus at:" ), focus_ );

	nucleus_ = make_real( this, QStringLiteral( "synthNucleus" ), PYRA_SYNTHETIC_NUCLEUS_MIN_UM,
		PYRA_SYNTHETIC_NUCLEUS_MAX_UM, defaults.nucleus_diameter_um, 1, tr( " µm" ) );
	nucleus_->setToolTip( tr( "Mean equivalent diameter of the tumour nuclei" ) );
	tissue_form->addRow( tr( "&Nucleus diameter:" ), nucleus_ );
	spacing_ = make_real( this, QStringLiteral( "synthSpacing" ), PYRA_SYNTHETIC_SPACING_MIN_UM,
		PYRA_SYNTHETIC_SPACING_MAX_UM, defaults.nucleus_spacing_um, 1, tr( " µm" ) );
	spacing_->setToolTip( tr( "Mean distance between tumour nuclei inside nests (√ area per nucleus)" ) );
	tissue_form->addRow( tr( "Nucleus s&pacing:" ), spacing_ );
	positive_ = make_percent( this, QStringLiteral( "synthPositive" ), defaults.positive_fraction );
	positive_->setToolTip( tr( "Expected share of DAB-positive (brown) tumour nuclei" ) );
	tissue_form->addRow( tr( "Positi&ve nuclei:" ), positive_ );
	tumour_ = make_percent( this, QStringLiteral( "synthTumour" ), defaults.tumour_fraction );
	tumour_->setToolTip( tr( "Expected share of the tissue area inside tumour nests" ) );
	tissue_form->addRow( tr( "T&umour area:" ), tumour_ );
	lymphocytes_ = make_percent( this, QStringLiteral( "synthLymphocytes" ), defaults.lymphocyte_fraction );
	lymphocytes_->setToolTip( tr( "Share of the stromal nuclei that are lymphocytes; the rest are fibroblasts" ) );
	tissue_form->addRow( tr( "L&ymphocytes in stroma:" ), lymphocytes_ );

	codec_ = new QComboBox( this );
	codec_->setObjectName( QStringLiteral( "synthCodec" ) );
	for (const auto &codec : kCodecs) {
		if (pyra_codec_can_encode( codec.value ))
			codec_->addItem( QString::fromUtf8( codec.label ), uint32_t( codec.value ) );
	}
	codec_->setCurrentIndex( std::max( 0, codec_->findData( defaults.codec ) ) );
	slide_form->addRow( tr( "&Compression:" ), codec_ );
	quality_ = make_spin( this, QStringLiteral( "synthQuality" ), 1, 100, int( defaults.quality ) );
	lossless_ = new QCheckBox( tr( "&Lossless" ), this );
	lossless_->setObjectName( QStringLiteral( "synthLossless" ) );
	auto *quality_row = new QHBoxLayout;
	quality_row->addWidget( quality_, 1 );
	quality_row->addWidget( lossless_ );
	slide_form->addRow( tr( "&Quality:" ), quality_row );
	layout->addLayout( form );
	layout->addWidget( slide_box );
	layout->addWidget( focus_box );
	layout->addWidget( tissue_box );

	auto *buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
	buttons->button( QDialogButtonBox::Ok )->setText( tr( "Generate" ) );
	layout->addWidget( buttons );
	const auto enable_ok = [this, buttons] {
		buttons->button( QDialogButtonBox::Ok )->setEnabled( !path_->text().trimmed().isEmpty() );
	};
	connect( path_, &QLineEdit::textChanged, this, enable_ok );
	connect( codec_, &QComboBox::currentIndexChanged, this, [this] { update_quality(); } );
	connect( lossless_, &QCheckBox::toggled, this, [this] { update_quality(); } );
	connect( buttons, &QDialogButtonBox::accepted, this, &QDialog::accept );
	connect( buttons, &QDialogButtonBox::rejected, this, &QDialog::reject );
	connect( browse, &QPushButton::clicked, this, [this] {
		const auto selected = QFileDialog::getSaveFileName( this, tr( "Save synthetic WSI" ),
			path_->text(), tr( "PTF slides (*.ptf)" ) );
		if (!selected.isEmpty())
			path_->setText( selected );
	} );
	enable_ok();
	update_quality();
}


QString GenerateWsiDialog::path () const
{
	return path_->text().trimmed();
}


pyra_synthetic_options GenerateWsiDialog::options () const
{
	auto options = pyra_synthetic_options{};
	pyra_synthetic_options_init( &options );
	options.width = uint32_t( width_->value() );
	options.height = uint32_t( height_->value() );
	options.tile_edge = uint32_t( tile_->value() );
	options.z_levels = uint32_t( z_levels_->value() );
	options.mpp = mpp_->value();
	options.z_step_um = z_step_->value();
	options.focus_fraction = focus_->value() / kPercent;
	options.nucleus_diameter_um = nucleus_->value();
	options.nucleus_spacing_um = spacing_->value();
	options.positive_fraction = positive_->value() / kPercent;
	options.tumour_fraction = tumour_->value() / kPercent;
	options.lymphocyte_fraction = lymphocytes_->value() / kPercent;
	options.codec = codec_->currentData().toUInt();
	options.quality = lossless_->isEnabled() && lossless_->isChecked()
		? PYRA_QUALITY_LOSSLESS : uint32_t( quality_->value() );
	return options;
}


// JPEG has no lossless mode; a lossless choice makes quality meaningless.
void GenerateWsiDialog::update_quality ()
{
	const auto can_lossless = codec_->currentData().toUInt() != PYRA_CODEC_JPEG;
	lossless_->setEnabled( can_lossless );
	quality_->setEnabled( !( can_lossless && lossless_->isChecked() ) );
}

} // namespace pyra::test
