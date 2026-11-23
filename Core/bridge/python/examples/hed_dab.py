# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT
"""Chunk pass: the DAB stain channel by colour deconvolution.

The DAB optical density (OD) is the one column of skimage's colour
deconvolution (skimage.color.rgb2hed) that this channel keeps:

    OD = sum over c in R, G, B of  log(max(c / c_max, 1e-6)) / log(1e-6) * M[c, DAB]

with M = skimage.color.hed_from_rgb. Each term depends on one channel's integer
level only, so it is a table with one float32 entry per level (256 for uint8
RGB), and a pixel costs three lookups and two additions. That gives the same
values as rgb2hed(chunk)[..., 2] within one output count (float32 rounding),
without rgb2hed's float64 copies of the chunk or its haematoxylin and eosin
planes.

DICOM WSI has no floating-point samples, so the DAB OD range 0 .. dab_max_od is
mapped linearly onto the whole output range (0 .. 65535 for gray16, 0 .. 255
for gray8) and anything denser is clipped to the top:

    sample = min(OD, dab_max_od) / dab_max_od * full_scale

so OD = sample / full_scale * dab_max_od. The default 0.3 keeps typical DAB well
inside the range, so the channel is visible as stored, while the rare very
dark pixels (debris, folds) saturate instead of setting the scale. dab_max_od
is a parameter, so it is recorded with the channel's processing details.
Outside the prepare pass's "roi" mask, when there is one, the result is 0.

    pyra_process slide.dcm --script hed_dab.py --channel DAB --type gray16 \\
        [--prepare-script tissue_prepare.py] [--param dab_max_od=0.3]
"""

import numpy as np
from skimage import color   # lazy: skimage.color loads (~0.3 s) on first use, in the first chunk

DAB = 2                 # column of hed_from_rgb
OD_FLOOR = 1e-6         # rgb2hed's floor before the log
dab_max_od = 0.3
tables = {}             # (source dtype, output dtype) -> three per-level tables


def setup(params):
    global dab_max_od
    dab_max_od = float(params.get("dab_max_od", dab_max_od))
    if not dab_max_od > 0:
        raise ValueError("dab_max_od must be a positive optical density")
    tables.clear()


def level_tables(source, output):
    # Each table already holds its share of the output sample, before clipping.
    key = (source, output)
    if key not in tables:
        top = np.iinfo(source).max
        levels = np.maximum(np.arange(top + 1) / top, OD_FLOOR)
        scale = np.iinfo(output).max / dab_max_od / np.log(OD_FLOOR)
        tables[key] = [(np.log(levels) * color.hed_from_rgb[c, DAB] * scale).astype(np.float32)
                       for c in range(3)]
    return tables[key]


def process(chunk, ctx):
    if chunk.ndim != 3 or chunk.shape[2] != 3:
        raise ValueError("hed_dab needs an RGB source plane")
    red, green, blue = level_tables(chunk.dtype, np.dtype(ctx.dtype))
    sample = red[chunk[..., 0]]
    sample += green[chunk[..., 1]]
    sample += blue[chunk[..., 2]]
    np.clip(sample, 0.0, np.iinfo(ctx.dtype).max, out=sample)
    result = np.rint(sample).astype(ctx.dtype)
    roi = ctx.masks.get("roi")
    if roi is not None:
        result[roi == 0] = 0
    return result
