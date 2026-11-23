# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT
"""Prepare pass: a tissue mask for the whole slide, from its coarse image.

Tissue absorbs light, so it is darker than the glass around it. One Otsu
threshold on the smoothed coarse image separates the two. The mask comes back as
the raster "roi": chunks it leaves empty are skipped, and process scripts see it
as ctx.masks["roi"]. "store=roi" keeps it as a channel of its own.

    pyra_process slide.dcm --prepare-script tissue_prepare.py ...
"""

import numpy as np
from skimage import color, filters


def prepare(image, ctx):
    gray = color.rgb2gray(image) if image.ndim == 3 else image / max(float(image.max()), 1.0)
    smooth = filters.gaussian(gray, sigma=float(ctx.params.get("tissue_sigma", "2")))
    threshold = float(filters.threshold_otsu(smooth))
    tissue = smooth < threshold
    print(f"tissue: threshold {threshold:.4f}, {tissue.mean():.1%} of {tissue.shape}")
    return {"roi": tissue}, {"tissue_threshold": f"{threshold:.6f}", "store": "roi"}
