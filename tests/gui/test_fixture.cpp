#include "test_fixture.h"

#include "ptf_write.h"

#include <QDir>

#include <algorithm>
#include <stdexcept>

namespace pyra::test {

namespace {
constexpr auto kFixtureTileEdge = 64;
}

QString write_test_slide ( const QString &directory )
{
	if (!QDir().mkpath( directory ))
		throw std::runtime_error( "Cannot create fixture output directory" );
	const auto path = QDir( directory ).filePath( QStringLiteral( "gui_fixture.ptf" ) );
	auto writer = priml::PtfWriter{};
	auto error = std::string{};
	if (!writer.open( path.toUtf8().constData(), &error ))
		throw std::runtime_error( error );
	for (const auto z : { 0, 2 }) {
		auto spec = priml::PtfLevelSpec{};
		spec.size = { kFixtureTileEdge * 4, kFixtureTileEdge * 3 };
		spec.tile = { kFixtureTileEdge, kFixtureTileEdge };
		spec.tags.valid = true;
		spec.tags.channel = "G";
		spec.tags.zlevel = float( z );
		spec.tags.mpp = 1;
		spec.tags.pixel_type = priml::LevelPixelType::Gray8;
		spec.tags.max_pixel_value = 255;
		spec.codec = priml::ImgCodec::Jpeg;
		spec.encode.quality = priml::kImageDefaultLossyQuality;
		auto fetch = priml::PtfTileFetch<uint8_t>(
			[z] ( const priml::Rtlbr<int> &rect, priml::Pwhl<uint8_t> *tile ) {
				const auto alternate = ( rect.tl.x / kFixtureTileEdge +
					rect.tl.y / kFixtureTileEdge ) % 2;
				const auto value = z == 0 ? ( alternate ? 220 : 37 ) : ( alternate ? 90 : 193 );
				std::fill( tile->buf.begin(), tile->buf.end(), uint8_t( value ) );
				return true;
			} );
		if (!writer.add_level( spec, fetch, &error ))
			throw std::runtime_error( error );
	}
#if USE_OPENJPEG
	auto spec = priml::PtfLevelSpec{};
	spec.size = { kFixtureTileEdge * 4, kFixtureTileEdge * 3 };
	spec.tile = { kFixtureTileEdge, kFixtureTileEdge };
	spec.tags.valid = true;
	spec.tags.channel = "DAPI";
	spec.tags.mpp = 1;
	spec.tags.pixel_type = priml::LevelPixelType::Gray16;
	spec.tags.max_pixel_value = 4095;
	spec.codec = priml::ImgCodec::Jp2k;
	auto fetch = priml::PtfTileFetch<uint16_t>(
		[] ( const priml::Rtlbr<int> &, priml::Pwhl<uint16_t> *tile ) {
			std::fill( tile->buf.begin(), tile->buf.end(), uint16_t( 2048 ) );
			return true;
		} );
	if (!writer.add_level( spec, fetch, &error ))
		throw std::runtime_error( error );
#endif
	if (!writer.close( &error ))
		throw std::runtime_error( error );
	return path;
}

} // namespace pyra::test
