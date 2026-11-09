#include "open_wsi_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace pyra::test {

OpenWsiDialog::OpenWsiDialog ( const QString &directory, const QString &path,
	const pyra_context_options &options, QWidget *parent )
	: QDialog( parent ), options_( options )
{
	setWindowTitle( tr( "Open WSI" ) );
	auto *layout = new QVBoxLayout( this );
	auto *form = new QFormLayout;
	auto *file_row = new QHBoxLayout;
	path_ = new QLineEdit( path, this );
	path_->setObjectName( QStringLiteral( "wsiPath" ) );
	path_->setAccessibleName( tr( "WSI file" ) );
	auto *browse = new QPushButton( tr( "Browse…" ), this );
	browse->setAutoDefault( false );
	file_row->addWidget( path_, 1 );
	file_row->addWidget( browse );
	form->addRow( tr( "WSI file:" ), file_row );
	backend_ = new QComboBox( this );
	backend_->setObjectName( QStringLiteral( "readerBackend" ) );
	for (const auto backend : { PYRA_BACKEND_NATIVE, PYRA_BACKEND_OPENSLIDE }) {
		auto info = pyra_backend_info{ sizeof( pyra_backend_info ), 0, nullptr, nullptr };
		if (pyra_get_backend_info( backend, &info, nullptr ) == PYRA_OK && info.available)
			backend_->addItem( QString::fromUtf8( info.name ), uint32_t( backend ) );
	}
	backend_->setCurrentIndex( std::max( 0, backend_->findData( options.backend ) ) );
	form->addRow( tr( "&Reader:" ), backend_ );
	const auto make_cache = [this] ( const QString &name, uint64_t bytes, int minimum ) {
		auto *spin = new QSpinBox( this );
		spin->setObjectName( name );
		spin->setRange( minimum, std::numeric_limits<int>::max() );
		spin->setSuffix( tr( " MiB" ) );
		spin->setValue( int( std::min<uint64_t>( bytes / kBytesPerMiB, uint64_t( spin->maximum() ) ) ) );
		return spin;
	};
	tbf_ = make_cache( QStringLiteral( "tbfCacheMiB" ), options.compressed_cache_bytes, 1 );
	pbf_ = make_cache( QStringLiteral( "pbfCacheMiB" ), options.decoded_cache_bytes, 0 );
	pbf_->setToolTip( tr( "Zero disables retained decoded tiles." ) );
	threads_ = new QSpinBox( this );
	threads_->setObjectName( QStringLiteral( "decodeThreads" ) );
	threads_->setRange( 1, std::numeric_limits<int>::max() );
	threads_->setValue( int( options.decode_threads ) );
	form->addRow( tr( "&TBF cache:" ), tbf_ );
	form->addRow( tr( "&PBF cache:" ), pbf_ );
	form->addRow( tr( "&Decoder threads:" ), threads_ );
	layout->addLayout( form );
	policy_ = new QLabel( this );
	policy_->setWordWrap( true );
	layout->addWidget( policy_ );
	auto *buttons = new QDialogButtonBox( QDialogButtonBox::Open | QDialogButtonBox::Cancel, this );
	layout->addWidget( buttons );
	const auto enable_open = [this, buttons] {
		buttons->button( QDialogButtonBox::Open )->setEnabled( !path_->text().trimmed().isEmpty() );
	};
	connect( path_, &QLineEdit::textChanged, this, enable_open );
	connect( backend_, &QComboBox::currentIndexChanged, this, [this] { update_backend(); } );
	connect( buttons, &QDialogButtonBox::accepted, this, &QDialog::accept );
	connect( buttons, &QDialogButtonBox::rejected, this, &QDialog::reject );
	connect( browse, &QPushButton::clicked, this, [this, directory] {
		const auto selected = QFileDialog::getOpenFileName( this, tr( "Choose WSI" ),
			path_->text().isEmpty() ? directory : path_->text(),
			tr( "Whole-slide images (*.svs *.ndpi *.bif *.ptf *.glr *.dcm *.czi *.tif *.tiff *.mrxs *.scn *.vms *.vmu *.svslide *.avs);;All files (*)" ) );
		if (!selected.isEmpty())
			path_->setText( selected );
	} );
	enable_open();
	update_backend();
}


QString OpenWsiDialog::path () const
{
	return path_->text();
}


pyra_context_options OpenWsiDialog::options () const
{
	auto options = options_;
	options.backend = backend_->currentData().toUInt();
	options.compressed_cache_bytes = uint64_t( tbf_->value() ) * kBytesPerMiB;
	options.decoded_cache_bytes = uint64_t( pbf_->value() ) * kBytesPerMiB;
	options.decode_threads = uint32_t( threads_->value() );
	return options;
}


void OpenWsiDialog::update_backend ()
{
	const auto native = backend_->currentData().toUInt() == PYRA_BACKEND_NATIVE;
	tbf_->setEnabled( native );
	threads_->setEnabled( native );
	policy_->setText( native ? tr( "Limits apply across all slides in the reader context. PBF 0 disables decoded caching." ) :
		tr( "OpenSlide uses the PBF limit. TBF and decoder-thread control are unavailable for this reader." ) );
}

} // namespace pyra::test
