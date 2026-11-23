// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "cache_usage_widget.h"

#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace pyra::test {
namespace {

constexpr auto kBarSteps = 1000;
constexpr auto kTbfColor = "#1976d2";
constexpr auto kPbfColor = "#2e7d32";

} // namespace


CacheUsageWidget::CacheUsageWidget ( QWidget *parent ) : QWidget( parent )
{
	setAccessibleName( tr( "Pyramid cache occupancy" ) );
	setToolTip( tr( "Occupied slots divided by currently allocated slots. Arena covers the whole pyramid; Level shows the level used by the last main-view render. Pools may grow up to their configured memory limits. Synthetic levels have no tile cache." ) );
	setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
	auto *layout = new QVBoxLayout( this );
	const auto inset = fontMetrics().horizontalAdvance( QLatin1Char( 'M' ) );
	layout->setContentsMargins( inset, 0, inset, 0 );
	const auto add_cache = [this, layout] ( const QString &name, const char *color,
		QLabel **text, QProgressBar **bar ) {
		*text = new QLabel( this );
		(*text)->setObjectName( name + QStringLiteral( "CacheAllocationText" ) );
		(*text)->setWordWrap( true );
		*bar = new QProgressBar( this );
		(*bar)->setObjectName( name + QStringLiteral( "CacheAllocation" ) );
		(*bar)->setAccessibleName( name.toUpper() + tr( " cache occupancy" ) );
		(*bar)->setRange( 0, kBarSteps );
		(*bar)->setTextVisible( false );
		(*bar)->setFixedHeight( std::max( 6, fontMetrics().height() / 2 ) );
		(*bar)->setStyleSheet( QStringLiteral(
			"QProgressBar { border: 1px solid palette(mid); background: palette(base); border-radius: 2px; }"
			"QProgressBar::chunk { background-color: %1; }" ).arg( QString::fromLatin1( color ) ) );
		layout->addWidget( *text );
		layout->addWidget( *bar );
	};
	add_cache( QStringLiteral( "tbf" ), kTbfColor, &tbf_text_, &tbf_bar_ );
	add_cache( QStringLiteral( "pbf" ), kPbfColor, &pbf_text_, &pbf_bar_ );
	set_usage( {} );
}


void CacheUsageWidget::set_usage ( const CacheUsage &usage )
{
	const auto show = [this, &usage] ( const QString &name, bool decoded, QLabel *text, QProgressBar *bar ) {
		bar->setValue( 0 );
		const auto limit = decoded ? usage.options.decoded_cache_bytes : usage.options.compressed_cache_bytes;
		auto state = QString{};
		if (!usage.image_open)
			state = tr( "no image" );
		else if (decoded && limit == 0)
			state = tr( "disabled" );
		else if (usage.options.backend == PYRA_BACKEND_OPENSLIDE)
			state = decoded ? tr( "%1 MiB capacity (OpenSlide); occupancy unavailable" )
				.arg( QLocale{}.toString( double( limit ) / kBytesPerMiB, 'g', 6 ) ) : tr( "unavailable (OpenSlide)" );
		else if (!usage.occupancy)
			state = tr( "unavailable" );
		else {
			const auto &counts = *usage.occupancy;
			const auto level_mode = counts.topology == PYRA_CACHE_PER_LEVEL;
			if (level_mode && counts.level_index < 0)
				state = tr( "awaiting first render" );
			else if (level_mode && counts.synthetic_level)
				state = tr( "synthetic level — no tile cache" );
			else {
				const auto used = decoded ? counts.decoded_used_slots : counts.compressed_used_slots;
				const auto allocated = decoded ? counts.decoded_allocated_slots : counts.compressed_allocated_slots;
				const auto locale = QLocale{};
				state = tr( "%1 / %2 slots used" ).arg( locale.toString( qulonglong( used ) ),
					locale.toString( qulonglong( allocated ) ) );
				if (allocated > 0) {
					const auto fraction = std::clamp( double( used ) / double( allocated ), 0.0, 1.0 );
					bar->setValue( int( std::lround( fraction * kBarSteps ) ) );
					state += tr( " (%1%)" ).arg( locale.toString( 100.0 * fraction, 'f', 1 ) );
				} else
					state += tr( " (not allocated)" );
			}
		}
		auto scope = QString{};
		if (usage.occupancy) {
			const auto &counts = *usage.occupancy;
			scope = counts.topology == PYRA_CACHE_ARENA ? tr( "Arena" ) :
				( counts.level_index >= 0 ? tr( "Level %1" ).arg( counts.level_index ) : tr( "Level" ) );
		}
		const auto description = name + ( scope.isEmpty() ? QString{} : QStringLiteral( " (%1)" ).arg( scope ) ) +
			QStringLiteral( ": " ) + state;
		text->setText( description );
		bar->setAccessibleDescription( description );
	};
	show( QStringLiteral( "TBF" ), false, tbf_text_, tbf_bar_ );
	show( QStringLiteral( "PBF" ), true, pbf_text_, pbf_bar_ );
}

} // namespace pyra::test
