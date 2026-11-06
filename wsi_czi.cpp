#include "build_config.h"

#if USE_CZI

// Zeiss CZI loader.
//
// A CZI is a single file holding a bag of sub-blocks. Each is an independently
// encoded rectangle placed at the position it was acquired at, so - unlike every
// other format here - they do not tile the slide. A mosaic scan overlaps its
// neighbours (Zeiss writes 10% by default) and a slide may carry several scenes
// with bare stage between them. That is fine: tiles are selected geometrically
// (select_tiles_for_rect in pyr.h), overlapping tiles simply paint in order, and
// a region with no sub-block is left as background.
//
// Only the structure is read here. The encoded sub-block bytes are handed to the
// shared codec layer behind a small descriptor (ImgCodec::CziSubBlock), so the
// expensive JPEG XR / zstd decode happens on the shared decode pool rather than
// serially while loading - the same division of labour the other loaders get for
// free from their JPEG tiles.
//
// One plane only: channel 0, z 0, t 0. Multi-channel compositing is a display
// question, not a loader one.

#include "img_codec.h"
#include "wsi.h"

#include "pyr_rt.h"
#include "pyr_win.h"

//
// libCZI's public headers put std::string / std::map / std::function members in
// dll-exported classes (C4251) and narrow an int to its enum's underlying type
// (C4244). Both are inside the library's own interface and not actionable here,
// and at /W4 they were some 6000 lines of build output per full rebuild - enough
// to bury our own warnings, which is how four real ones went unnoticed.
#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable : 4251 4244 4458 4864 )
#endif
#include <libCZI.h>
#ifdef _MSC_VER
#pragma warning( pop )
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _MSC_VER
 #ifdef _DEBUG
  #pragma comment(lib, "libCZId.lib")
 #else
  #pragma comment(lib, "libCZI.lib")
 #endif
#endif

namespace priml {

// libCZI.dll is delay-loaded, so an installation without it starts normally and
// only the CZI format is unavailable - the DLL is needed by one format out of
// six. The catch is that the delay-load helper raises a structured exception on
// the first call into a DLL it cannot find, which is not something the try/catch
// blocks below would see. So ask first, once, and refuse the format cleanly.
//
// The handle is deliberately not released: the delay-load thunk is about to want
// the module anyway, and holding it saves a second search.
// Checked by WsiLoader::Create BEFORE a CziLoader is constructed, because the
// constructor is already too late: CziState holds a libCZI::CDimCoordinate, and
// building one calls into the DLL. Asking here instead turns a missing runtime
// into "no loader for this extension" rather than a 0xC06D007E crash.
//
// Off Windows libCZI is an ordinary shared-library dependency: a missing one
// stops the process from starting at all, so once running it is there.
bool czi_runtime_available ()
{
#ifdef _WIN32
	static const auto available = [] {
#ifdef _DEBUG
		const auto *name = L"libCZId.dll";
#else
		const auto *name = L"libCZI.dll";
#endif
		if (::LoadLibraryW( name ))
			return true;
		pyr::plog_err(
			"wsi czi: %ls is not present beside the executable - CZI slides "
			"cannot be opened; every other format is unaffected\n", name );
		return false;
	}();
	return available;
#else
	return true;
#endif
}


// Per-level libCZI state, kept index-parallel to pyramid.levels. sub_blocks is
// parallel to Level::tiles, mapping a tile back to the sub-block it came from;
// Tile::foff carries the same index, the way wsi_dcm.cpp carries a pixel-item
// index there.
struct CziState {
	std::shared_ptr<libCZI::IStream> stream;
	std::shared_ptr<libCZI::ICZIReader> reader;
	libCZI::CDimCoordinate plane;
	libCZI::PixelType pixel_type = libCZI::PixelType::Invalid;
	float black = 0.0f;
	float white = 1.0f;
	std::vector<std::vector<int>> sub_blocks;
};


// One pyramid layer under construction: the sub-blocks sharing a zoom.
// A sub-block on its way to becoming a tile. The index is what ReadSubBlock
// wants; the file position is kept because the gap to the next segment bounds
// how many bytes this one can occupy - see EncodedSizeBounds.
struct CziPendingBlock {
	libCZI::SubBlockInfo info;
	std::uint64_t file_position = 0;
	int index = 0;
	size_t encoded_bound = 0;
};


struct CziPendingLevel {
	double zoom = 1.0;
	std::vector<CziPendingBlock> blocks;
};


// Sub-block zooms are computed from integer sizes, so the layer-1 blocks of one
// file land on 0.5 and 0.499999. Quantising to a power of two keeps a layer
// together; anything that is not a clean power of two keeps its own bucket.
static double QuantiseZoom ( double zoom )
{
	if (!( zoom > 0.0 ) || zoom > 1.0)
		return zoom > 1.0 ? 1.0 : zoom;

	auto exponent = std::lround( std::log2( zoom ) );
	auto snapped = std::pow( 2.0, (double)exponent );
	return std::abs( snapped - zoom ) <= zoom * 0.02 ? snapped : zoom;
}


// X/Y microns per pixel from the CZI metadata. libCZI reports METRES.
static PixelMetric ReadPixelMetric ( libCZI::ICZIReader *reader )
{
	try {
		auto segment = reader->ReadMetadataSegment();
		if (!segment)
			return {};
		auto metadata = segment->CreateMetaFromMetadataSegment();
		if (!metadata || !metadata->IsXmlValid())
			return {};
		auto info = metadata->GetDocumentInfo();
		if (!info)
			return {};
		auto scaling = info->GetScalingInfo();
		auto metric = PixelMetric{};
		if (scaling.IsScaleXValid())
			metric.x = (float)( scaling.scaleX * 1e6 );
		if (scaling.IsScaleYValid())
			metric.y = (float)( scaling.scaleY * 1e6 );
		return metric;
	}
	catch (const std::exception &) {
		return {};
	}
}


// Significant bits per component of channel 0 - 12 or 14 for a camera writing
// into a 16-bit container. 0 when the metadata does not say.
static int ReadComponentBitCount ( libCZI::ICZIReader *reader )
{
	try {
		auto segment = reader->ReadMetadataSegment();
		if (!segment)
			return 0;
		auto metadata = segment->CreateMetaFromMetadataSegment();
		if (!metadata || !metadata->IsXmlValid())
			return 0;
		auto info = metadata->GetDocumentInfo();
		auto channels = info ? info->GetDimensionChannelsInfo() : nullptr;
		auto channel = channels && channels->GetChannelCount() > 0
			? channels->GetChannel( 0 ) : nullptr;
		auto bits = 0;
		if (!channel || !channel->TryGetComponentBitCount( &bits ))
			return 0;
		return bits;
	}
	catch (const std::exception &) {
		return 0;
	}
}


// Display black/white points for channel 0, normalised 0..1. Returns false when
// the file carries none - the caller then derives a window from the data, since
// full range on a 14-bit sensor reading is a black picture.
static bool ReadBlackWhitePoint ( libCZI::ICZIReader *reader,
	float *black, float *white )
{
	*black = 0.0f;
	*white = 1.0f;
	auto found = false;
	try {
		auto segment = reader->ReadMetadataSegment();
		if (!segment)
			return false;
		auto metadata = segment->CreateMetaFromMetadataSegment();
		if (!metadata || !metadata->IsXmlValid())
			return false;
		auto info = metadata->GetDocumentInfo();
		if (!info)
			return false;
		auto display = info->GetDisplaySettings();
		if (!display)
			return false;
		auto channel = display->GetChannelDisplaySettings( 0 );
		if (!channel)
			return false;
		auto lo = 0.0f;
		auto hi = 1.0f;
		channel->GetBlackWhitePoint( &lo, &hi );
//
// A full-range pair is what libCZI reports when the element is absent as well as
// when it genuinely says 0..1, and the two are indistinguishable here. Treating
// it as "not stated" costs nothing: deriving 0..1 from data that really occupies
// the full range gives 0..1 back.
		if (hi > lo && !( lo <= 0.0f && hi >= 1.0f )) {
			*black = lo;
			*white = hi;
			found = true;
		}
	}
	catch (const std::exception &) {
	}
	return found;
}


// Derive a display window from the pixels when the file states none.
//
// A camera writing 12- or 14-bit samples into a 16-bit container uses a fraction
// of the range - test_gray.czi peaks at 12.2% of full scale - so mapping 0..65535
// straight to the screen renders it black. Percentiles rather than min/max, so a
// single hot pixel or a dead column cannot flatten everything else.
static bool AutoWindowFromData ( libCZI::ICZIReader *reader,
	const std::vector<CziPendingLevel> &levels, float *black, float *white )
{
	if (levels.empty() || levels.back().blocks.empty())
		return false;

//
// The coarsest level: fewest sub-blocks, and each is a decimation of the whole
// slide rather than one corner of it.
//
// EVERY sub-block of it, not a sample of them. Sampling four was wrong: on a
// slide whose blocks differ widely - a test pattern with one constant per block
// ranged from 7000 to 47115 - the four that happened to be picked decided the
// container depth for the whole file, and picking low ones clips every bright
// block. The pixel stride does the thinning instead, which spreads the budget
// across all of them rather than concentrating it in a few.
	const auto &source = levels.back().blocks;
	constexpr auto kMaxSampledPixels = 4ull << 20;
	auto histogram = std::vector<uint64_t>( 65536, 0 );
	auto counted = uint64_t{ 0 };

	try {
		auto total = uint64_t{ 0 };
		for (const auto &block : source) {
			total += (uint64_t)block.info.physicalSize.w *
				(uint64_t)block.info.physicalSize.h;
		}
//
// One stride for the whole level, chosen so the total sample stays bounded
// whatever the slide's size or block count.
		auto stride = 1u;
		while (total / ( (uint64_t)stride * stride ) > kMaxSampledPixels)
			++stride;

		for (size_t i = 0; i < source.size(); ++i) {
			auto block = reader->ReadSubBlock( source[i].index );
			if (!block)
				continue;
			auto bitmap = block->CreateBitmap();
			if (!bitmap)
				continue;

			auto type = bitmap->GetPixelType();
			auto size = bitmap->GetSize();
			auto locker = libCZI::ScopedBitmapLockerSP{ bitmap };
			const auto *base = (const uint8_t *)locker.ptrDataRoi;
			if (!base)
				continue;

			for (auto y = 0u; y < size.h; y += stride) {
				const auto *row = base + (size_t)y * (size_t)locker.stride;
				for (auto x = 0u; x < size.w; x += stride) {
					auto v = uint16_t{ 0 };
					switch (type) {
						case libCZI::PixelType::Gray8:
							v = (uint16_t)( row[x] << 8 | row[x] );
							break;
						case libCZI::PixelType::Gray16:
							v = (uint16_t)( row[x * 2] | ( row[x * 2 + 1] << 8 ) );
							break;
						case libCZI::PixelType::Gray32Float: {
							auto f = 0.0f;
							std::memcpy( &f, row + (size_t)x * 4, sizeof( f ) );
							v = (uint16_t)std::clamp(
								f * 65535.0f, 0.0f, 65535.0f );
							break;
						}
						default:
							return false;
					}
					++histogram[v];
					++counted;
				}
			}
		}
	}
	catch (const std::exception &) {
		return false;
	}

	if (counted == 0)
		return false;

//
// The window is the sample's own container depth, not a percentile stretch.
//
// A camera writing 12- or 14-bit samples into a 16-bit word leaves the top bits
// clear, so the picture is dark for one plain reason - the data occupies a known
// fraction of the range - and scaling by that fraction is what any grey-image
// viewer does. A percentile stretch instead maps the observed spread across the
// full output, which on a near-uniform field clips half the image to white; that
// was tried and it did exactly that.
//
// The high end comes from a percentile rather than the true maximum so that a
// few hot pixels cannot promote the whole image to the next depth. Zero is
// excluded: unimaged stage between mosaic tiles is exactly zero.
	auto imaged = counted - histogram[0];
	if (imaged == 0)
		return false;

	auto high_target = (uint64_t)( imaged * 0.999 );
	auto running = uint64_t{ 0 };
	auto peak = 65535;
	for (auto v = 1; v < 65536; ++v) {
		running += histogram[v];
		if (running > high_target) {
			peak = v;
			break;
		}
	}

	auto depth_max = 65535;
	if (peak < 256)
		depth_max = 255;
	else if (peak < 4096)
		depth_max = 4095;
	else if (peak < 16384)
		depth_max = 16383;

//
// Distinct levels in what libCZI itself decoded. This is the reference against
// the CZSB path: a flat result here means the file really is flat, whereas
// structure here and flatness on screen means the fault is ours.
	auto distinct = 0;
	for (auto v = 1; v < 65536; ++v)
		distinct += histogram[v] ? 1 : 0;

	pyr::plog( "wsi czi: grey sample (libCZI decode): %llu px, %d distinct levels, "
		"99.9%% at %d -> %d-bit container\n",
		(unsigned long long)imaged, distinct, peak,
		depth_max == 255 ? 8 : depth_max == 4095 ? 12
			: depth_max == 16383 ? 14 : 16 );

	if (depth_max >= 65535)
		return false;			// already full range; nothing to correct

	*black = 0.0f;
	*white = (float)depth_max / 65535.0f;
	return true;
}


// Decode a whole auxiliary CZI - the Label and SlidePreview attachments are
// themselves small CZI files - into an owning image.
template <typename pixeltype>
static bool DecodeNestedCzi ( const void *data, size_t size,
	Pwhl<pixeltype> &out )
{
	try {
		auto blob = std::shared_ptr<const void>(
			std::malloc( size ), [] ( const void *p ) {
				std::free( const_cast<void *>( p ) );
			} );
		if (!blob)
			return false;
		std::memcpy( const_cast<void *>( blob.get() ), data, size );

		auto reader = libCZI::CreateCZIReader();
		reader->Open( libCZI::CreateStreamFromMemory( blob, size ) );
		auto accessor = reader->CreateSingleChannelScalingTileAccessor();
		auto box = reader->GetStatistics().boundingBox;
		if (box.w <= 0 || box.h <= 0)
			return false;

		auto plane = libCZI::CDimCoordinate{ { libCZI::DimensionIndex::C, 0 } };
		auto bitmap = accessor->Get( libCZI::PixelType::Bgr24,
			libCZI::IntRectAndFrameOfReference{
				libCZI::CZIFrameOfReference::RawSubBlockCoordinateSystem, box },
			&plane, 1.0f, nullptr );
		if (!bitmap)
			return false;

		auto size2 = bitmap->GetSize();
		out = Pwhl<pixeltype>( { (int)size2.w, (int)size2.h } );
		if (!out.p)
			return false;

		auto locker = libCZI::ScopedBitmapLockerSP{ bitmap };
		const auto *base = (const uint8_t *)locker.ptrDataRoi;
		for (auto y = 0; y < (int)size2.h; ++y) {
			const auto *src = base + (size_t)y * (size_t)locker.stride;
			auto *row = out.rowp( y );
			for (auto x = 0; x < (int)size2.w; ++x, src += 3)
				row[x] = PixelFromRgbBytes<pixeltype>( src[2], src[1], src[0] );
		}
		return true;
	}
	catch (const std::exception &) {
		return false;
	}
}


// Slide label and macro overview, carried as attachments rather than sub-blocks.
// Zeiss writes them as embedded CZI files; other writers use a plain JPEG, which
// the shared codec layer identifies from its own magic bytes.
static void ReadAuxiliaryImages ( libCZI::ICZIReader *reader,
	Ppyr *pyramid )
{
	auto wanted = std::vector<std::pair<std::string, Pwhl<Rgb> *>>{
		{ "Label", &aux_slot( *pyramid, kAuxFrost ) },
		{ "SlidePreview", &aux_slot( *pyramid, kAuxMacro ) } };

	try {
		reader->EnumerateAttachments(
			[&] ( int index, const libCZI::AttachmentInfo &info ) {
				for (auto &[name, target] : wanted) {
					if (info.name != name || target->is_set())
						continue;

					auto attachment = reader->ReadAttachment( index );
					if (!attachment)
						continue;
					const void *data = nullptr;
					auto size = size_t{ 0 };
					attachment->DangerousGetRawData( data, size );
					if (!data || size == 0)
						continue;

					auto type = std::string( info.contentFileType );
					if (type == "CZI")
						DecodeNestedCzi<Rgb>( data, size, *target );
					else
						decode_to_image<Rgb>(
							(const uint8_t *)data, size, *target );
				}
				return true;
			} );
	}
	catch (const std::exception &e) {
		pyr::plog( "wsi czi: attachment read failed: %s\n", e.what() );
	}
}


bool czi_measure_subblock_read ( const std::string &path, int max_sub_blocks,
	CziReadCost *out )
{
	if (!out || !czi_runtime_available())
		return false;
	*out = CziReadCost{};

	try {
		auto reader = libCZI::CreateCZIReader();
		reader->Open( libCZI::CreateStreamFromFile(
			pyr::str2wstr( path ).c_str() ) );

//
// Layer-0 sub-blocks only: those are the ones a full-resolution pan reads, and
// the ones whose size makes the cost visible.
		auto picked = std::vector<std::pair<int, uint64_t>>{};
		reader->EnumerateSubBlocksEx(
			[&] ( int index, const libCZI::DirectorySubBlockInfo &info ) {
				if (info.GetZoom() > 0.99 &&
					(int)picked.size() < max_sub_blocks)
				{
					picked.emplace_back( index, info.filePosition );
				}
				return (int)picked.size() < max_sub_blocks;
			} );
		if (picked.empty())
			return false;

		auto sizes = std::vector<size_t>( picked.size(), 0 );

//
// Warm-up: pull every one through once so neither pass pays for cold disk or a
// cold sub-block directory. What is being compared is steady-state overhead.
		for (size_t i = 0; i < picked.size(); ++i) {
			auto block = reader->ReadSubBlock( picked[i].first );
			if (!block)
				return false;
			const void *data = nullptr;
			auto size = size_t{ 0 };
			block->DangerousGetRawData( libCZI::ISubBlock::Data, data, size );
			sizes[i] = size;
		}

		auto t0 = std::chrono::steady_clock::now();
		for (const auto &[index, position] : picked) {
			(void)position;
			auto block = reader->ReadSubBlock( index );
			const void *data = nullptr;
			auto size = size_t{ 0 };
			block->DangerousGetRawData( libCZI::ISubBlock::Data, data, size );
//
// Touch a byte so the read cannot be optimised away, without letting it into the
// byte count.
			if (size)
				(void)*(const volatile uint8_t *)data;
			out->bytes += size;
		}
		out->libczi_us = std::chrono::duration<double, std::micro>(
			std::chrono::steady_clock::now() - t0 ).count();

		auto file = std::ifstream( path, std::ios::binary );
		if (!file)
			return false;
//
// Allocated once at the largest size. Resizing per iteration would value-
// initialise the buffer each time, charging the plain-read side a memset of
// every byte that libCZI never pays - which is measurement noise, not I/O.
		auto buffer = std::vector<char>(
			*std::max_element( sizes.begin(), sizes.end() ) );
		auto t1 = std::chrono::steady_clock::now();
		for (size_t i = 0; i < picked.size(); ++i) {
			file.seekg( (std::streamoff)picked[i].second );
			file.read( buffer.data(), (std::streamsize)sizes[i] );
		}
		out->ifstream_us = std::chrono::duration<double, std::micro>(
			std::chrono::steady_clock::now() - t1 ).count();

//
// The third arm: Win32 ReadFile with the offset carried in an OVERLAPPED, which
// is exactly what libCZI's own stream does (StreamImpl.cpp). Same bytes, same
// destination buffer, no CRT stream layer in between - so the gap between this
// and the ifstream arm is that layer's cost, and the gap between this and libCZI
// is whatever libCZI does beyond the read itself. Windows only; elsewhere
// readfile_us stays 0 and the comparisons against it are skipped.
#ifdef _WIN32
		auto handle = ::CreateFileW( pyr::str2wstr( path ).c_str(), GENERIC_READ,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr );
		if (handle != INVALID_HANDLE_VALUE) {
			auto t2 = std::chrono::steady_clock::now();
			for (size_t i = 0; i < picked.size(); ++i) {
				auto overlapped = OVERLAPPED{};
				overlapped.Offset = (DWORD)( picked[i].second & 0xFFFFFFFFull );
				overlapped.OffsetHigh = (DWORD)( picked[i].second >> 32 );
				auto read = DWORD{ 0 };
				::ReadFile( handle, buffer.data(), (DWORD)sizes[i],
					&read, &overlapped );
			}
			out->readfile_us = std::chrono::duration<double, std::micro>(
				std::chrono::steady_clock::now() - t2 ).count();
			::CloseHandle( handle );
		}
#endif

		out->sub_blocks = (int)picked.size();
		return true;
	}
	catch (const std::exception &e) {
		pyr::plog( "wsi czi: read-cost measurement failed: %s\n", e.what() );
		return false;
	}
}


CziLoader::CziLoader () : state_( std::make_unique<CziState>() ) {}


CziLoader::~CziLoader () = default;


// Group the plane's sub-blocks into pyramid layers by zoom, finest first.
// How many bytes each sub-block can occupy on disk, from the gaps between
// segment start positions.
//
// This matters more than it looks: Tile::fsz sizes the TileBf slot, and sizing
// it from the DECODED raster gave 14.5 MiB slots for ~840 KB payloads. Twenty-odd
// of those exhausted the pool budget on their own, so every tile load evicted
// something first - which is where 42.8 ms per load came from, against the 224 us
// the read itself takes. A segment carries a header and metadata before its data,
// so the gap to the next segment is a true upper bound on the data.
static void EncodedSizeBounds ( std::vector<CziPendingLevel> *levels,
	uint64_t file_size )
{
	auto starts = std::vector<uint64_t>{};
	for (const auto &level : *levels) {
		for (const auto &block : level.blocks)
			starts.push_back( block.file_position );
	}
	if (starts.empty())
		return;
	std::sort( starts.begin(), starts.end() );

	for (auto &level : *levels) {
		for (auto &block : level.blocks) {
			auto it = std::upper_bound(
				starts.begin(), starts.end(), block.file_position );
			auto next = it != starts.end() ? *it : file_size;
			auto gap = next > block.file_position
				? (size_t)( next - block.file_position ) : 0;
//
// The decoded size is also a valid bound - an encoder that grew the data would
// have stored it uncompressed - so take whichever is tighter, and never zero.
			auto decoded = (size_t)block.info.physicalSize.w *
				(size_t)block.info.physicalSize.h *
				(size_t)libCZI::Utils::GetBytesPerPixel( block.info.pixelType );
			block.encoded_bound = gap > 0 ? std::min( gap, decoded ) : decoded;
		}
	}
}


static std::vector<CziPendingLevel> CollectCziLevels (
	libCZI::ICZIReader *reader )
{
	auto by_zoom = std::map<double, CziPendingLevel, std::greater<double>>{};

	reader->EnumerateSubBlocksEx(
		[&] ( int index, const libCZI::DirectorySubBlockInfo &info ) {
//
// One plane only. A coordinate the file does not use reads as absent, so this
// keeps single-channel brightfield and channel 0 of a fluorescence stack alike.
			auto value = 0;
			auto on_first_plane = [&] ( libCZI::DimensionIndex dim ) {
				return !info.coordinate.TryGetPosition( dim, &value ) || value == 0;
			};
			if (!on_first_plane( libCZI::DimensionIndex::C ) ||
				!on_first_plane( libCZI::DimensionIndex::Z ) ||
				!on_first_plane( libCZI::DimensionIndex::T ))
			{
				return true;
			}
			if (info.logicalRect.w <= 0 || info.logicalRect.h <= 0 ||
				info.physicalSize.w == 0 || info.physicalSize.h == 0)
			{
				return true;
			}

			auto zoom = QuantiseZoom( info.GetZoom() );
			auto &level = by_zoom[zoom];
			level.zoom = zoom;
			level.blocks.push_back( CziPendingBlock{
				static_cast<const libCZI::SubBlockInfo &>( info ),
				info.filePosition, index, 0 } );
			return true;
		} );

	auto levels = std::vector<CziPendingLevel>{};
	levels.reserve( by_zoom.size() );
	for (auto &[zoom, level] : by_zoom) {
		(void)zoom;
		levels.push_back( std::move( level ) );
	}
	return levels;
}


bool CziLoader::Open ( Ppyr &pyramid )
{
	if (!czi_runtime_available())
		return false;

	try {
		auto path = pyr::str2wstr( this->filename_ );
		state_->stream = libCZI::CreateStreamFromFile( path.c_str() );
		state_->reader = libCZI::CreateCZIReader();
		state_->reader->Open( state_->stream );
	}
	catch (const std::exception &e) {
		pyr::plog( "wsi czi: %s: open failed: %s\n",
			this->filename_.c_str(), e.what() );
		return false;
	}

	auto *reader = state_->reader.get();
	auto statistics = reader->GetStatistics();
	auto base_box = statistics.boundingBoxLayer0Only;
	if (base_box.w <= 0 || base_box.h <= 0) {
		pyr::plog( "wsi czi: %s: empty layer-0 bounding box\n",
			this->filename_.c_str() );
		return false;
	}

	auto source_metric = ReadPixelMetric( reader );
	pyramid.source_pixel_metric = source_metric;
	auto base_mpp = source_metric.x;
	if (base_mpp <= 0.0f) {
		pyr::plog( "wsi czi: %s: no pixel size in metadata, assuming 1 um\n",
			this->filename_.c_str() );
		base_mpp = 1.0f;
	}
	auto window_from_metadata = ReadBlackWhitePoint(
		reader, &state_->black, &state_->white );

	state_->plane = libCZI::CDimCoordinate{
		{ libCZI::DimensionIndex::C, 0 },
		{ libCZI::DimensionIndex::Z, 0 },
		{ libCZI::DimensionIndex::T, 0 } };

	auto pending = CollectCziLevels( reader );
	{
		auto ec = std::error_code{};
		auto file_size = std::filesystem::file_size( this->filename_, ec );
		EncodedSizeBounds( &pending, ec ? 0 : (uint64_t)file_size );
	}
	if (pending.empty()) {
		pyr::plog( "wsi czi: %s: no sub-blocks on the first plane\n",
			this->filename_.c_str() );
		return false;
	}

	if (!pending.front().blocks.empty())
		state_->pixel_type = pending.front().blocks.front().info.pixelType;

//
// Grey planes only, and only when the file said nothing: a colour image needs no
// window, and a stated one is the author's intent and outranks anything measured.
	auto is_grey = state_->pixel_type == libCZI::PixelType::Gray8 ||
		state_->pixel_type == libCZI::PixelType::Gray16 ||
		state_->pixel_type == libCZI::PixelType::Gray32Float;
	if (is_grey && !window_from_metadata) {
		if (AutoWindowFromData( reader, pending, &state_->black, &state_->white )) {
			pyr::plog( "wsi czi: %s: no display settings; window derived from the "
				"data at %.4f..%.4f of full scale\n",
				this->filename_.c_str(), state_->black, state_->white );
		} else {
			pyr::plog( "wsi czi: %s: no display settings and none could be "
				"derived; using full range\n", this->filename_.c_str() );
		}
	}

//
// 48-bit colour is declared as such, at the camera's own depth: the tiles keep
// their samples rather than being cut to 8 bits, and a 12-bit camera's 0..4095
// says so instead of reading as a very dark 16-bit image.
	const auto deep_colour = state_->pixel_type == libCZI::PixelType::Bgr48;
	auto deep_max = 0;
	if (deep_colour) {
		const auto bits = ReadComponentBitCount( reader );
		deep_max = bits > 8 && bits <= 16 ? ( 1 << bits ) - 1 : 0;
	}

	state_->sub_blocks.clear();
	for (auto &pl : pending) {
		auto level = Level{};
		level.metric = base_mpp / (float)pl.zoom;
		if (deep_colour) {
			level.pixel_type = LevelPixelType::Rgb16;
			level.pixel_type_declared = true;
			level.max_pixel_value = deep_max;
		}

		auto indices = std::vector<int>{};
		indices.reserve( pl.blocks.size() );
		auto max_tile_w = 0;
		auto max_tile_h = 0;
		auto max_fsz = size_t{ 0 };

//
// Paint order within a layer: ascending M, so a later-acquired mosaic tile wins
// the overlap band, matching what libCZI's own compositor does with sortByM.
// set_level_bounds then sorts by (top, left) for selection, which is stable, so
// blocks at the same origin keep this order.
		std::stable_sort( pl.blocks.begin(), pl.blocks.end(),
			[] ( const CziPendingBlock &a, const CziPendingBlock &b ) {
				return a.info.mIndex < b.info.mIndex;
			} );

		for (const auto &entry : pl.blocks) {
			const auto &block = entry.info;
			auto tile = Tile{};
//
// logicalRect is in layer-0 pixels; scale it into this level and shift it so the
// level starts at the origin. physicalSize is what the payload decodes to, and
// the two agree to within the rounding libCZI itself does.
			auto x0 = (int)std::lround(
				( block.logicalRect.x - base_box.x ) * pl.zoom );
			auto y0 = (int)std::lround(
				( block.logicalRect.y - base_box.y ) * pl.zoom );
			tile.lr = { { x0, y0 },
				{ x0 + (int)block.physicalSize.w,
				  y0 + (int)block.physicalSize.h } };
			tile.foff = (size_t)indices.size();
//
// The ENCODED bound, not the decoded raster: this is the TileBf slot size, and
// sizing it from the raster reserved 14.5 MiB per 840 KB payload.
			tile.fsz = (size_t)kCziTileHeaderBytes + entry.encoded_bound;

			max_tile_w = std::max( max_tile_w, (int)block.physicalSize.w );
			max_tile_h = std::max( max_tile_h, (int)block.physicalSize.h );
			max_fsz = std::max( max_fsz, tile.fsz );
			indices.push_back( entry.index );
			level.tiles.push_back( std::move( tile ) );
		}

		if (state_->pixel_type == libCZI::PixelType::Invalid &&
			!pl.blocks.empty())
		{
			state_->pixel_type = pl.blocks.front().info.pixelType;
		}

		level.tile_w = max_tile_w;
		level.tile_h = max_tile_h;
		set_level_bounds( level );
//
// set_level_bounds reorders the tiles, so the sub-block indices have to be
// permuted with them: Tile::foff is the position each tile held in `indices`.
		auto ordered = std::vector<int>( indices.size() );
		for (size_t i = 0; i < level.tiles.size(); ++i)
			ordered[i] = indices[level.tiles[i].foff];
		for (size_t i = 0; i < level.tiles.size(); ++i)
			level.tiles[i].foff = i;

		level.tbf.init( (int)max_fsz, (int)level.tiles.size() );

		pyramid.levels.push_back( std::move( level ) );
		state_->sub_blocks.push_back( std::move( ordered ) );
	}

	pyramid.mrct = { { 0, 0 },
		{ base_box.w * base_mpp, base_box.h * base_mpp } };

//
// Ask for the decoded-tile cache when the payloads are compressed. A Zeiss
// JPEG XR sub-block measures ~119 ms to decode and a mosaic render touches only
// one or two of them, so there is nothing for the decode pool to parallelise and
// every repaint pays in full. Uncompressed sub-blocks are just a copy, so they
// are better served by the same rule as JPEG: spend the RAM on more tiles.
	pyramid.prefer_pixbf = false;
	for (const auto &pl : pending) {
		for (const auto &entry : pl.blocks) {
			if (entry.info.GetCompressionMode() !=
				libCZI::CompressionMode::UnCompressed)
			{
				pyramid.prefer_pixbf = true;
				break;
			}
		}
		if (pyramid.prefer_pixbf)
			break;
	}

	ReadAuxiliaryImages( reader, &pyramid );

	pyr::plog( "wsi czi: %s: %zu levels, %dx%d px, %.4f um/px, pixel type %s, "
		"prefers pixbf=%s\n",
		this->filename_.c_str(), pyramid.levels.size(),
		base_box.w, base_box.h, base_mpp,
		libCZI::Utils::PixelTypeToInformalString( state_->pixel_type ),
		pyramid.prefer_pixbf ? "cache" : "bypass" );
	for (size_t i = 0; i < pyramid.levels.size(); ++i) {
		const auto &level = pyramid.levels[i];
		pyr::plog( "wsi czi:   L%zu metric=%.4f tiles=%zu max_tile=%dx%d\n",
			i, level.metric, level.tiles.size(), level.tile_w, level.tile_h );
	}
	return true;
}


bool CziLoader::LoadTile ( Ppyr &pyramid, int level_no,
	int tile_no, unsigned worker_idx )
{
	return this->LoadCompressedTile( pyramid, level_no, tile_no, worker_idx,
		[this, level_no] ( auto &level, auto &tile, uint8_t *dest, size_t &pos,
			unsigned /*worker*/ ) {
			(void)level;
			if (level_no < 0 || level_no >= (int)state_->sub_blocks.size())
				return false;
			const auto &indices = state_->sub_blocks[level_no];
			if (tile.foff >= indices.size())
				return false;

			try {
				auto block = state_->reader->ReadSubBlock( indices[tile.foff] );
				if (!block)
					return false;

				const void *data = nullptr;
				auto size = size_t{ 0 };
				block->DangerousGetRawData(
					libCZI::ISubBlock::Data, data, size );
				if (!data || size == 0)
					return false;
				if (size + (size_t)kCziTileHeaderBytes > tile.fsz)
					return false;

				const auto &info = block->GetSubBlockInfo();
				auto w = (uint32_t)info.physicalSize.w;
				auto h = (uint32_t)info.physicalSize.h;

				dest[0] = 'C';  dest[1] = 'Z';  dest[2] = 'S';  dest[3] = 'B';
				dest[4] = kCziTileHeaderVersion;
				dest[5] = (uint8_t)info.GetCompressionMode();
				dest[6] = (uint8_t)info.pixelType;
				dest[7] = 0;
				std::memcpy( dest + 8, &w, sizeof( w ) );
				std::memcpy( dest + 12, &h, sizeof( h ) );
				std::memcpy( dest + 16, &state_->black, sizeof( float ) );
				std::memcpy( dest + 20, &state_->white, sizeof( float ) );
				std::memcpy( dest + kCziTileHeaderBytes, data, size );
				pos = (size_t)kCziTileHeaderBytes + size;
				return true;
			}
			catch (const std::exception &e) {
				pyr::plog( "wsi czi: sub-block read failed: %s\n", e.what() );
				return false;
			}
		} );
}


void CziLoader::Close ( Ppyr &pyramid )
{
	state_->sub_blocks.clear();
	if (state_->reader) {
		try {
			state_->reader->Close();
		}
		catch (const std::exception &) {
		}
	}
	state_->reader.reset();
	state_->stream.reset();
	this->CloseFile();
	pyramid.levels.clear();
}



} // namespace priml

#endif
