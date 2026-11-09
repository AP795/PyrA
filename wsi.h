#pragma once

#include "pyr.h"
#include "wsi_diag.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace priml {

// Persistent thread pool for JPEG decode workers.
// Calling thread participates as worker 0; pool threads handle 1..N-1.
class DecodePool {
public:
	~DecodePool ();
	void run_batch ( unsigned worker_count, std::function<void(unsigned)> fn );
	void shutdown ();
	void set_file ( const std::string &filename );
//
// Handle for one worker on one file. `path` empty selects the pyramid's own
// file, which is what every level of an ordinary single-file slide uses; a
// level that names its own file (Level::filename) gets its own row.
//
// Rows hold unique_ptrs so that growing the table never moves an ifstream a
// concurrent worker is already reading through.
	std::ifstream &worker_file ( unsigned pool_idx, const std::string &path = {} );

private:
	std::vector<std::thread> threads_;
	std::map<std::string, std::vector<std::unique_ptr<std::ifstream>>> file_handles_;
	std::mutex file_handles_mtx_;
	std::string filename_;
	std::function<void(unsigned)> *work_fn_ = nullptr;
	std::exception_ptr batch_exception_;
	std::mutex batch_mtx_;
	std::mutex mtx_;
	std::condition_variable cv_start_;
	std::condition_variable cv_done_;
	unsigned generation_ = 0;
	unsigned batch_size_ = 0;
	unsigned done_ = 0;
	bool stop_ = false;

	void ensure_workers ( unsigned count );
	void worker_loop ( unsigned pool_idx );
};


const char *JpegDecoderName ();
void LogPixelMetricInequality ( const std::string &filename,
	const Ppyr &pyramid );

// What pixel type a file's levels decode to, without opening the pyramid.
//
// Reads the first declared level type. Integer monochrome opens retain both
// Gray8 and Gray16 channels; other pixel families are selected by OpenFile<T>.
// Returns false when the file does not declare one.
bool ProbeWsiPixelType ( const std::string &filename, LevelPixelType *out );

// The DICOM half of ProbeWsiPixelType, defined in wsi_dcm.cpp (USE_DCMTK builds).
bool ProbeDcmPixelType ( const std::string &filename, LevelPixelType *out );

// The gallery half: the manifest's pixel-type line, defined in wsi_glr.cpp.
bool ProbeGalleryPixelType ( const std::string &filename, LevelPixelType *out );

// The named integer channels of a file, in file order, without opening the pyramid:
// what a slide's base planes are when they are not R, G and B of an ordinary colour
// image. Empty for every file that names no channel. A gallery's come from its
// manifest, a PTF's from its plane-identity blocks.
std::vector<std::string> ProbeWsiChannels ( const std::string &filename );

// Select the requested pixel family and assign its fallback type to undeclared
// levels. Gray8 and Gray16 belong to one integer monochrome family.
bool ResolveWsiPixelTypes ( Ppyr &pyramid, LevelPixelType fallback );

// To decode a standalone compressed buffer into an owning Pwhl (e.g. the DICOM
// label/overview), call priml::decode_to_image in img_codec.h - it sniffs the
// encoding rather than assuming JPEG.

// One open slide file.
//
// Not templated: opening a file, reading its metadata and pulling a compressed
// tile off disk are all type-agnostic - the bytes are bytes. Only the members
// that touch DECODED pixels carry a pixel type, and they are member templates.
// That is what lets one slide hold levels of several types behind one loader.
class WsiLoader {
public:
	std::string filename_;
	DecodePool decode_pool_;

	virtual ~WsiLoader() = default;

// T selects the pixel family and supplies the type for undeclared levels.
	template <typename pixeltype>
	bool OpenFile ( const std::string &filename, Ppyr &pyramid ) {
		filename_ = filename;
		pyramid.source_pixel_metric = {};
		file_.close();
		file_.clear();
		file_.open( pyr::path_from_utf8( filename_ ), std::ios::binary );
		if (!file_)
			return false;
		decode_pool_.set_file( filename_ );
		auto t0 = std::chrono::steady_clock::now();
		auto ok = Open( pyramid );
		if (ok)
			LogPixelMetricInequality( filename_, pyramid );
//
// Resolve the selected pixel family, then group its channels into typed ladders.
		if (ok)
			ok = ResolveWsiPixelTypes(
				pyramid, pixel_type_of<pixeltype>::value );
		if (ok) {
//
// Every opened level has one concrete decoded-pixel type. Materialise that
// small type-erased owner while opening is still serialized, before a shared
// session can hand the pyramid to concurrent zone workers.
			for (auto &level : pyramid.levels) {
				visit_level_pixel_type( level.pixel_type, [&] ( auto pixel ) {
					(void)level_pix<decltype( pixel )>( level );
				} );
			}
		}
		if (ok)
			rebuild_plane_index( pyramid );
//
// Top the pyramid up with coarse levels the file does not carry, so a zoomed-out
// view never has to read the whole base level at once. Shared by every loader
// rather than repeated per format - it works off Level geometry alone.
		if (ok) {
			TopUpCoarseLevels<pixeltype>( pyramid );
			if (auto geometry = display_native_geometry( pyramid ))
				pyramid.mrct = geometry->metric_bounds;
//
// The loader has now said whether its tiles are expensive to decode; honour
// that if the caller asked for auto rather than fixing the mode by config.
			if (pyramid.use_pixbf_auto)
				pyramid.use_pixbf = pyramid.prefer_pixbf;
//
// Last, so the level geometry and auxiliary images it reports are the final
// ones - the coarse top-up above adds levels.
			set_property( pyramid, "pyra.vendor", VendorName() );
			record_derived_properties( pyramid );
		}
		auto dt = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - t0 ).count();
		pyramid.stats.open_ms = dt;
		if (!ok)
			CloseFile();
		return ok;
	}

	virtual void Close ( Ppyr &pyramid ) = 0;

// Short name of the format this loader reads, for pyra.vendor.
	virtual const char *VendorName () const = 0;

// Load + Assemble: reads raw data from file into TileBf slot with header prepended.
// Format-specific (virtual). Must set tile.ninbuf, tile.bf_sz, tile.sts = Loaded.
// worker_idx selects the file handle (0 = main, 1+ = pool per-thread handles).
	virtual bool LoadTile ( Ppyr &pyramid, int level_no, int tile_no,
		unsigned worker_idx = 0 ) = 0;

// Shared JPEG decompression: reads assembled JPEG from TileBf, decodes into PixBf.
// Called from Generate(). Sets tile.nixbuf, tile.pbd (non-owning), tile.sts = Ready.
	template <typename pixeltype>
	bool DecodeTile ( Ppyr &pyramid, int level_no, int tile_no );

// Samples the full requested metric rectangle; out-of-bounds pixels are zero.
// Each destination axis spans that rectangle independently.
// When target_size is given (non-null, positive), the result is produced at
// exactly those pixel dimensions instead of Generate deriving them from the
// subrect. Callers can supply a shared raster size to keep image, mask, results,
// and display dimensions consistent.
	template <typename pixeltype>
	Pwhl<pixeltype> Generate ( Ppyr &pyramid,
		Render<pixeltype> *render,
		float microns_per_pixel,
		const Rtlbr<float> &subrect,
		const Dim2D<int> *target_size = nullptr,
		const PlaneSel &sel = {} );

// Returns source-level pixels clipped to the level bounds, without resampling
// or padding, as a non-owning span into a caller-owned persistent render cache.
	template <typename pixeltype>
	Pwhl<pixeltype> GenerateSpan ( Ppyr &pyramid,
		Render<pixeltype> &render,
		float microns_per_pixel,
		const Rtlbr<float> &subrect,
		const PlaneSel &sel = {} );

	int AcquireTileBfSlot ( Ppyr &pyramid, int level_no, int tile_no );
	template <typename pixeltype>
	int AcquirePixBfSlot ( Ppyr &pyramid, int level_no, int tile_no );
	void ReleaseTileBfSlot ( Ppyr &pyramid, int level_no, int slot );
	void ReleasePixBfSlot ( Ppyr &pyramid, int level_no, int slot );

// Factory: creates the appropriate loader based on file extension (.ndpi, .svs, ...)
	static std::unique_ptr<WsiLoader> Create ( const std::string &filename );

protected:
	virtual bool Open ( Ppyr &pyramid ) = 0;

// Appends decimated levels when the file's own coarsest is still too large to
// render whole. Defined in wsi.cpp; a no-op for a slide with a proper pyramid.
	template <typename pixeltype>
	void TopUpCoarseLevels ( Ppyr &pyramid );

// Helper: read `count` bytes from file at `file_offset` into `dest`.
// worker_idx 0 uses the main file_, 1+ use decode_pool_ per-thread handles.
	bool ReadFromFile ( uint8_t *dest, size_t file_offset, size_t count,
		unsigned worker_idx = 0, const std::string &path = {} );

	template <typename AssembleFn>
	bool LoadCompressedTile ( Ppyr &pyramid, int level_no, int tile_no,
		unsigned worker_idx, AssembleFn assemble )
	{
		if (level_no >= (int)pyramid.levels.size())
			return false;

		auto &level = pyramid.levels[level_no];
		if (tile_no >= (int)level.tiles.size())
			return false;

		auto &tile = level.tiles[tile_no];
		if (tile.fsz == 0) {
//
// A zero-byte tile is a legitimately-empty (sparse) region of the pyramid —
// some encoders omit blank tiles — not a load error. Mark it Ready with no
// pixel buffer so DecodeTile short-circuits and Generate skips its area
// (leaving the render background) instead of failing the tile and re-loading /
// logging it on every frame.
			tile.sts.store( TileSts::Ready, std::memory_order_release );
			return true;
		}

		while (true) {
			auto sts = tile.sts.load( std::memory_order_acquire );
			if (sts == TileSts::Ready || sts == TileSts::Loaded)
				return true;
			if (sts == TileSts::Loading || sts == TileSts::Decoding) {
				std::this_thread::yield();
				continue;
			}

			auto expected = TileSts::Empty;
			if (tile.sts.compare_exchange_weak( expected, TileSts::Loading,
				std::memory_order_acq_rel, std::memory_order_acquire )) {
				break;
			}
		}

		auto slot = AcquireTileBfSlot( pyramid, level_no, tile_no );
		if (slot < 0) {
			pyr::plog(
				"wsi tile fail: stage=tilebf-alloc lvl=%d tile=%d rect=[%d,%d]-[%d,%d] "
				"foff=%zu fsz=%zu alloc=%zu used=%d/%d cap=%zu ram_cap=%zu reason=failed to reserve compressed tile buffer slot\n",
				level_no, tile_no,
				tile.lr.tl.x, tile.lr.tl.y, tile.lr.br.x, tile.lr.br.y,
				tile.foff, tile.fsz,
				level.tbf.allocated_bytes(), level.tbf.used_slots, level.tbf.num_slots,
				pyramid.tbf_cap_bytes, pyramid.ram_cap_bytes );
			tile.sts.store( TileSts::Empty, std::memory_order_release );
			return false;
		}

		std::unique_lock tbf_guard( *level.tbf_mtx_ );
		auto *dest = level.tbf.slot_ptr( slot );
		auto pos = size_t{0};

		if (!assemble(level, tile, dest, pos, worker_idx)) {
			pyr::plog(
				"wsi tile fail: stage=load lvl=%d tile=%d rect=[%d,%d]-[%d,%d] "
				"foff=%zu fsz=%zu slot=%d reason=assemble/load failed\n",
				level_no, tile_no,
				tile.lr.tl.x, tile.lr.tl.y, tile.lr.br.x, tile.lr.br.y,
				tile.foff, tile.fsz, slot );
			tbf_guard.unlock();
			ReleaseTileBfSlot( pyramid, level_no, slot );
			tile.sts.store( TileSts::Empty, std::memory_order_release );
			return false;
		}
		tbf_guard.unlock();

		tile.ninbuf = slot;
		tile.bf_sz = pos;
		tile.sts.store( TileSts::Loaded, std::memory_order_release );
		return true;
	}

	void CloseFile () {
		file_.close();
		file_.clear();
	}

// LRU eviction: free the least-recently-used TileBf slot across all levels.
// Returns true if a slot was freed, false if nothing to evict.
	bool EvictTileBf ( Ppyr &pyramid );

// LRU eviction: free the least-recently-used PixBf slot across all levels.
	bool EvictPixBf ( Ppyr &pyramid );

private:
	std::ifstream file_;
	mutable std::mutex file_mtx_;
	mutable std::mutex tilebf_alloc_mtx_;
	mutable std::mutex pixbf_alloc_mtx_;
	template <bool Decoded, typename Pool>
	int AcquireBufferSlot ( Ppyr &pyramid, int level_no, int tile_no, Pool &pool );

	size_t AllocatedPoolBytes ( const Ppyr &pyramid ) const;
//
// Bytes a level may add to one of its pools now, against the per-kind cap, its
// plane's budget and the pyramid-wide cap. A query, never an eviction - see the
// definition for why those had to be separated.
	size_t GrowthBudget ( const Ppyr &pyramid, int level_no, size_t wanted,
		size_t kind_cap, size_t kind_allocated ) const;
};


// Opaque RAII owner for a libtiff handle. Keeping TIFF out of this public
// header preserves the standalone loader interface and confines third-party
// headers to the TIFF implementation files.
class TiffHandle {
public:
	TiffHandle () = default;
	~TiffHandle ();

	TiffHandle ( const TiffHandle & ) = delete;
	TiffHandle &operator= ( const TiffHandle & ) = delete;

	bool open ( const std::string &filename );
	void close () noexcept;
	[[nodiscard]] void *get () const noexcept { return tif_; }

private:
	void *tif_ = nullptr;
};


class NdpiLoader : public WsiLoader {
public:
	const char *VendorName () const override { return "hamamatsu"; }
	void Close ( Ppyr &pyramid ) override;
	bool LoadTile ( Ppyr &pyramid, int level_no, int tile_no,
		unsigned worker_idx = 0 ) override;

protected:
	bool Open ( Ppyr &pyramid ) override;

private:
	TiffHandle tif_;
};

class SvsLoader : public WsiLoader {
public:
	const char *VendorName () const override { return "aperio"; }
	void Close ( Ppyr &pyramid ) override;
	bool LoadTile ( Ppyr &pyramid, int level_no, int tile_no,
		unsigned worker_idx = 0 ) override;

protected:
	bool Open ( Ppyr &pyramid ) override;

private:
	TiffHandle tif_;
};

class WsiTifLoader : public WsiLoader {
public:
	const char *VendorName () const override { return "generic-tiff"; }
	void Close ( Ppyr &pyramid ) override;
	bool LoadTile ( Ppyr &pyramid, int level_no, int tile_no,
		unsigned worker_idx = 0 ) override;

protected:
	bool Open ( Ppyr &pyramid ) override;

private:
	TiffHandle tif_;
};

// DICOM VL Whole Slide Microscopy loader.
// A slide is a folder of .dcm files sharing one SeriesInstanceUID; each VOLUME
// file is one pyramid level (TILED_FULL or TILED_SPARSE grid of JPEG-encoded
// frames). DcmState
// (defined in wsi_dcm.cpp) holds the per-level DCMTK handles, kept out of this
// header so DCMTK does not leak into the rest of the tree.
struct DcmState;

class DcmLoader : public WsiLoader {
public:
	const char *VendorName () const override { return "dicom"; }
	DcmLoader ();
	~DcmLoader () override;

	void Close ( Ppyr &pyramid ) override;
	bool LoadTile ( Ppyr &pyramid, int level_no, int tile_no,
		unsigned worker_idx = 0 ) override;

protected:
	bool Open ( Ppyr &pyramid ) override;

private:
	std::unique_ptr<DcmState> state_;
};

// Zeiss CZI loader.
//
// A CZI is one file holding a bag of sub-blocks, each an independently encoded
// rectangle placed at its acquisition position. They are NOT a grid: a mosaic
// scan overlaps neighbours (10% is typical) and a slide may hold several scenes
// with empty stage between them, so this loader leans on the geometric tile
// selection in BuildRenderPlan rather than fighting it into rows and columns.
// Levels are the pyramid layers the file itself stores, grouped by sub-block
// zoom. CziState (defined in wsi_czi.cpp) holds the libCZI handles, keeping
// libCZI out of this header.
struct CziState;

// True when libCZI.dll can be loaded. It is delay-loaded, so an installation
// without it runs normally and only loses the CZI format - but the first call
// into a missing delay-loaded DLL raises a structured exception, so this has to
// be asked before a CziLoader is built rather than after.
bool czi_runtime_available ();


// Diagnostic: what a sub-block actually costs to fetch.
//
// Rendering a CZI showed tile_load at ~40 ms per sub-block, on par with its JPEG
// XR decode and orders above the 20 us an NDPI strip takes - far more than the
// byte count justifies. This times libCZI's ReadSubBlock against reading the
// same byte ranges with a plain ifstream, both on a warm cache, so file I/O and
// libCZI's own overhead can be told apart.
struct CziReadCost {
	int sub_blocks = 0;
	size_t bytes = 0;
	double libczi_us = 0.0;		// ReadSubBlock + DangerousGetRawData
	double ifstream_us = 0.0;	// std::ifstream seekg + read
	double readfile_us = 0.0;	// Win32 ReadFile with an OVERLAPPED offset
};

bool czi_measure_subblock_read ( const std::string &path, int max_sub_blocks,
	CziReadCost *out );

// Gallery loader: a ".glr" manifest naming a directory of independent plain
// images (see gallery.h). Every member is one level-0 tile at its rectangle on a
// sparse mosaic; the pixels are read from the member files themselves, decoded by
// libtiff at load time and handed on as a RawSamples payload. Coarse levels are
// synthesised at open by the shared top-up. GalleryState (wsi_glr.cpp) holds the
// manifest.
struct GalleryState;

class GalleryLoader : public WsiLoader {
public:
	const char *VendorName () const override { return "gallery"; }
	GalleryLoader ();
	~GalleryLoader () override;

	void Close ( Ppyr &pyramid ) override;
	bool LoadTile ( Ppyr &pyramid, int level_no, int tile_no,
		unsigned worker_idx = 0 ) override;

protected:
	bool Open ( Ppyr &pyramid ) override;

private:
	std::unique_ptr<GalleryState> state_;
};

class CziLoader : public WsiLoader {
public:
	const char *VendorName () const override { return "zeiss"; }
	CziLoader ();
	~CziLoader () override;

	void Close ( Ppyr &pyramid ) override;
	bool LoadTile ( Ppyr &pyramid, int level_no, int tile_no,
		unsigned worker_idx = 0 ) override;

protected:
	bool Open ( Ppyr &pyramid ) override;

private:
	std::unique_ptr<CziState> state_;
};

} // namespace priml
