# Python processing bridge (`pyra_process`)

`pyra_process` runs your own Python code (numpy, scikit-image, anything Python
can import) over a whole DICOM WSI slide. It stores the result as a new channel
of that slide. The slide's existing files are never rewritten: the channel is a
set of new instance files published into the slide's series, and removing it
deletes exactly those files.

```sh
pyra_process slide.dcm --prepare-script tissue_prepare.py \
    --script hed_dab.py --channel DAB --type gray16 --halo 16
pyra_process slide.dcm --list
pyra_process slide.dcm --remove-channel DAB --remove-channel DAB.roi
```

The bridge is built with `PYRA_BUILD_BRIDGE` (default ON); it writes into
slides only when `PYR_WITH_DCMTK` is on, and otherwise reports that DICOM
support is missing. It needs Python 3 with numpy, plus whatever your
scripts import. The interpreter is taken from `--python`, then `$PYRA_PYTHON`,
then `python` (Windows) or `python3` on the `PATH`. The worker
(`python/pyra_worker.py`) and the examples (`python/examples/`) are copied next
to the program.

## How a run works

1. **Prepare pass (optional, `--prepare-script`).**
   - One Python process receives the whole source plane at a coarse resolution.
     By default the longest edge is at most 4096 px; `--prepare-mpp` sets the
     resolution instead.
   - The script returns named masks and `name=value` parameters.
   - This is where global decisions belong: a threshold computed over the whole
     slide, or a tissue mask.
2. **Chunk pass (`--script`).**
   - The level-0 plane is cut into chunks of whole source frames. By default a
     chunk is the power-of-two number of frames nearest 2048 px; `--chunk-tiles`
     sets it.
   - Worker threads each drive one Python process, `--workers` of them (half the
     cores by default). The processes start side by side: each start waits for
     Python and the script's imports, about 0.2 s with numpy alone.
   - For every chunk, a worker sends the chunk's pixels plus `--halo` pixels of
     context on each side, together with that chunk's window of each mask.
   - Python returns the processed pixels. The worker crops the halo, cuts the
     result into frames and encodes them losslessly (HTJ2K, or JPEG 2000 with
     `--codec jp2k`).
   - The worker also builds the chunk's reduced levels from the result it holds,
     halving it with a 2×2 mean, and cuts and encodes those too. It stops at the
     coarsest level whose frames still lie inside one chunk. A chunk of 2^K
     frames holds whole frames of K reduced levels, so a 4-frame chunk gives
     levels 1 and 2. That is why the default chunk is a power of two; with an
     explicit `--chunk-tiles 6`, only level 1 is built in the workers.
   - Masks the prepare script asked to keep (`store=`) are cut and encoded the
     same way, level by level, from their nearest-neighbour window. Frames of
     zeros are left out.
   - A chunk skipped for Python, but where a kept mask is non-zero, still runs as
     a mask-only job, without calling Python. So a kept mask is complete
     everywhere, not only where the script ran.
   - The main thread writes the encoded frames to a temporary spool as they
     arrive, in any order.
3. **Assembly.**
   - The spooled frames become the new instances, plane by plane and level by
     level. They are never decoded or re-encoded on the way.
   - The levels coarser than a chunk are rendered from the coarsest spooled
     level, halving until a level fits one frame. They cover 1/4^K of the
     level-0 area, so this serial step stays small.
   - Masks the prepare script asked to keep become channels the same way.
   - Reduced levels built in the workers are sparse, like level 0: a frame is
     absent where its whole chunk was skipped.
   - Everything is written into a hidden staging folder beside the slide, then
     moved into the slide's companion folder (`_<slide>_/`, or beside the slide
     for a flat vendor folder). An existing file is never replaced.

If anything fails before the last step, the staging folder is deleted and the
slide folder is exactly as before. The causes include a Python exception, a
wrong shape or dtype, and a name clash. The error message carries the Python
traceback.

Chunk workers run with `OMP_NUM_THREADS`, `OPENBLAS_NUM_THREADS` and
`MKL_NUM_THREADS` set to 1, because N worker processes each starting one BLAS
thread per core would compete for the CPU. The single prepare process keeps the
default. Source reads are serialized per slide by the reader (decoding inside a
read stays parallel); the per-phase timings printed at the end show whether
that matters for a given script.

## Script interface

### Prepare script

```python
def prepare(image, ctx):
    # image: H x W x 3 uint8 (RGB) or H x W (gray) - the whole slide, coarse
    # ctx.mpp: microns per pixel of image; ctx.params: dict of str (--param)
    return rasters, params
```

- **`rasters`** maps names to H × W `bool` or `uint8` arrays, at the prepare
  image's size. A `bool` array becomes 0/255.
- **`params`** maps names to strings. Every chunk worker receives them in
  `ctx.params`.
- **`roi`.** A raster with this name gates the chunk pass: a chunk whose `roi`
  window is all zero is skipped, with no Python call, and its frames stay
  absent. Chunks with no source frame are skipped too.
- **`store`.** A parameter with this name lists rasters to keep, comma
  separated (`"store": "roi"`). Each one becomes a gray8 channel
  `<channel>.<raster>` at full level-0 geometry, built frame by frame. All-zero
  frames are left absent, so nothing slide-sized is ever held in memory. The
  other rasters are discarded after the run.
- **Name clashes.** If a prepare parameter and a `--param` share a name, the run
  stops with an error; neither silently overrides the other.

### Chunk script

```python
def setup(params):            # optional, once per worker
    ...

def process(chunk, ctx):
    # chunk: H x W x 3 or H x W array of the source plane, halo included
    # ctx.x0, ctx.y0: level-0 pixel of chunk[0, 0]
    # ctx.mpp: microns per pixel        ctx.halo: (left, top, right, bottom)
    # ctx.params: dict of str           ctx.masks: name -> H x W uint8 window
    # ctx.chunk_id                    ctx.dtype: "uint8" or "uint16" (--type)
    return result               # H x W, of ctx.dtype
```

- **Result shape.** The result must have the chunk's full H × W, halo included;
  the worker crops the halo.
- **Result dtype.** It must match `--type` exactly. A `bool` result is accepted
  for `gray8` and becomes 0/255.
- **Output types.** DICOM WSI stores unsigned 8- or 16-bit samples only, so the
  bridge offers no float32 output. Map a float result onto the integer range
  yourself and make the mapping a parameter, so it is recorded with the
  channel.
- **Choosing the mapping.** Viewers scale a channel's display to the largest
  value it holds, so let the values you care about fill the range and clip
  rare extremes rather than letting them set the scale.
  - `hed_dab.py` maps DAB optical density 0 .. `dab_max_od` (default 0.3) onto
    the whole range of `ctx.dtype` and clips denser pixels.
  - Before, it stored density × 10000. On a synthetic IHC slide, the 99.9th
    percentile of DAB was then about 1,600, while a few dark pixels reached
    14,800, so the channel displayed black.
- **Memory per chunk.** Several workers share one memory bus, so a script's
  temporary arrays cost more than their compute.
  - `skimage.color.rgb2hed` makes float64 copies of the whole chunk, about
    100 MB each for a 2048² RGB chunk. It also computes haematoxylin and eosin
    even when only DAB is kept. A float32 input does not help: the stain matrix
    is float64, so the product is promoted back.
  - `hed_dab.py` therefore computes only the DAB column. Each channel's term
    depends on its integer level alone, so it is a float32 table with one entry
    per level, and a pixel costs three lookups and two additions. The result
    matches `rgb2hed` within one output count.
- **Printing.** `print()` is safe: anything a script writes to stdout goes to
  the log, because stdout carries the protocol.

### Masks in chunks

- Masks stay at prepare resolution. Each chunk receives its window,
  nearest-neighbour sampled onto level-0 pixels: level-0 column `x` reads coarse
  column `floor((x + 0.5) * coarse_width / level0_width)`, and rows the same
  way.
- The pixels are identical to upsampling the whole mask first, without ever
  allocating it.

### Halo and global operations

- **Neighbourhood operations.** Their output is exact across chunk seams when
  their support fits in `--halo`. Examples: filters, morphology, colour
  deconvolution.
- **Global operations.** Run per chunk, they give per-chunk answers: an Otsu
  threshold per chunk, or label numbers that restart in each chunk. Compute
  global values in the prepare pass and pass them on as parameters.

## What the new instances record

- **Identity.** Each new instance repeats the slide's Study, Series, Pyramid and
  Frame-of-Reference UIDs, so the reader shows it as one more plane of the same
  slide. Its optical path copies the slide's illumination under a new
  identifier and description (the channel name).
- **Type.** Image Type is `DERIVED`.
- **Processing record.** The `PROCESSING` acquisition-context item holds one
  line. It starts with `pyra-derived-channel` and is followed by:
  - the script names and SHA-256 hashes;
  - the prepare resolution;
  - the source plane, chunk size and halo;
  - every parameter;
  - for a stored mask, its name.

  Every level carries this record, not only the base.

## Removal

`--remove-channel NAME` deletes the instances of plane `NAME` only when every
one of them meets all of the following:

- it carries the writer identity (`dcm_identity.h`);
- its processing record starts with `pyra-derived-channel`;
- it is not the slide's entry file.

The writer identity and a processing record are not enough on their own: a
converter records its own processing (registration, flat field) on acquired
channels too. An acquired channel is therefore refused, and nothing is deleted.
`--list` shows every instance and which ones are derived.

## Wire protocol

Host and worker exchange messages over the worker's stdin and stdout.
`Core/bridge/bridge_protocol.h` defines them and `pyra_worker.py` mirrors that
definition. Each message has three parts:

1. a 64-byte little-endian header: magic `PYRA`, version, kind, chunk id,
   origin, size, sample type and channels, halo, text and payload lengths, and
   one double (mpp, or the Python time of a result);
2. UTF-8 `key=value` lines;
3. the samples.

The message kinds are:

| Kind | Sent when |
|---|---|
| Hello | The worker has started. |
| Setup / Ready | Before a worker's first chunk. |
| Prepare / Rasters | The prepare pass runs. |
| Chunk / Result | For each chunk. |
| Shutdown | The run ends. |
| Error | The worker failed (carries the traceback). |

## From the web viewer

The localhost viewer (`ViewW/`) has an **Analyse…** button for an open
DICOM slide. It offers the scripts of a scripts folder, the channel, output
type, halo, Python workers and parameters, and two ways to apply:
- **by CLI:** the server starts `pyra_process --progress-lines`;
- **by server:** the server calls `process_dcm_chunks` itself.

Both modes produce the same report (`format_chunk_report`). When the run
succeeds, the slide reloads with the view and plane kept, and the timing is
shown. See the web viewer's README for its endpoints and options.

The processed channel states the largest value the script returned as its
effective maximum (`RANGE_MAX`), so viewers scale its display to the data. A
DAB density scaled into a few thousand counts would otherwise show as black in
a 0..65535 range.

## Mixed pixel types in one slide

A gray result channel can join an RGB slide because the reader keeps planes of
another pixel family on open. The open's pixel type comes from the slide's own
first level. Each plane is rendered with its own type: the C API reads it from
the plane information.

## Not supported yet

- **PTF slides.** Appending a channel to a PTF needs a different publication
  scheme, since a TIFF cannot gain files the way a DICOM series can.
- **RLE Lossless and 1-bit DICOM Segmentation output.** The reader decodes
  neither.
- **A GUI entry.** `process_dcm_chunks` (`Core/bridge/chunk_process.h`) is the API a
  viewer would call.
