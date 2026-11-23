# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT
"""PyrA bridge worker: runs a user script for pyra_process.

    python -u pyra_worker.py prepare <script.py>
    python -u pyra_worker.py process <script.py>

The host speaks to this process over stdin/stdout in the binary format of
bridge/bridge_protocol.h. Anything the user script prints goes to stderr, which
the host forwards to its log: stdout belongs to the protocol.

Prepare script:
    prepare(image, ctx) -> (rasters, params)
        image   H x W x 3 (RGB) or H x W (gray) array, the whole slide, coarse
        ctx     .mpp (microns per pixel of image), .params (dict of str)
        rasters dict name -> H x W bool or uint8 array (bool becomes 0/255)
        params  dict name -> str; "store" lists rasters to keep as channels

Process script:
    setup(params)               optional, once per worker
    process(chunk, ctx) -> array
        chunk   H x W x 3 or H x W array of the source plane, halo included
        ctx     .x0, .y0      level-0 pixel of chunk[0, 0]
                .mpp          microns per pixel
                .halo         (left, top, right, bottom) context pixels
                .params       dict of str: prepare parameters and --param
                .masks        dict name -> H x W uint8 window of each raster
                .chunk_id
                .dtype        the output sample type: "uint8" or "uint16"
        returns an H x W array of the output dtype (uint8 or uint16; bool is
        accepted for uint8 and becomes 0/255). The halo is cropped here.
"""

import hashlib
import importlib.util
import os
import platform
import struct
import sys
import time
import traceback
import types

HEADER = struct.Struct("<4sHHIiiiiBB2xiiiiIId")
MAGIC = b"PYRA"
VERSION = 1
HELLO, SETUP, READY, PREPARE, RASTERS, CHUNK, RESULT, SHUTDOWN, ERROR = range(1, 10)
SAMPLES = {1: "uint8", 2: "uint16"}
SAMPLE_CODES = {name: code for code, name in SAMPLES.items()}
OUTPUT_DTYPE_KEY = "__dtype"


class Channel:
    def __init__(self):
        # The protocol keeps the real stdout; print() and stray writes go to stderr.
        self.out = os.fdopen(os.dup(1), "wb", buffering=0)
        os.dup2(2, 1)
        sys.stdout = sys.stderr
        self.inp = sys.stdin.buffer

    def read_exact(self, n):
        data = self.inp.read(n) if n else b""
        if len(data) != n:
            raise EOFError
        return data

    def receive(self):
        fields = HEADER.unpack(self.read_exact(HEADER.size))
        if fields[0] != MAGIC or fields[1] != VERSION:
            raise RuntimeError("malformed message from host (protocol mismatch?)")
        (_, _, kind, ident, x0, y0, width, height, sample, channels,
         hl, ht, hr, hb, text_bytes, payload_bytes, value) = fields
        text = self.read_exact(text_bytes).decode("utf-8")
        payload = self.read_exact(payload_bytes)
        return types.SimpleNamespace(kind=kind, id=ident, x0=x0, y0=y0, width=width,
                                     height=height, sample=sample, channels=channels,
                                     halo=(hl, ht, hr, hb), text=text, payload=payload,
                                     value=value)

    def send(self, kind, text="", payload=b"", ident=0, width=0, height=0, sample=0,
             channels=0, value=0.0):
        data = text.encode("utf-8")
        header = HEADER.pack(MAGIC, VERSION, kind, ident, 0, 0, width, height, sample,
                             channels, 0, 0, 0, 0, len(data), len(payload), value)
        self.out.write(header)
        if data:
            self.out.write(data)
        if payload:
            self.out.write(payload)


def parse_fields(text):
    fields = []
    for line in text.split("\n"):
        key, sep, value = line.partition("=")
        if sep and key:
            fields.append((key, value))
    return fields


def format_fields(fields):
    lines = []
    for key, value in fields:
        key, value = str(key), str(value)
        if not key or "=" in key or "\n" in key or "\r" in key or "\n" in value or "\r" in value:
            raise ValueError(f"name or value {key!r} contains '=', a line break, or is empty")
        lines.append(f"{key}={value}\n")
    return "".join(lines)


def load_script(path):
    # No __pycache__ beside the user's script: it often sits next to the slide,
    # whose folder a run must leave as it found it. Compiling a script is cheap.
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("pyra_user_script", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, os.path.dirname(os.path.abspath(path)))
    spec.loader.exec_module(module)
    return module


def image_from(np, message):
    dtype = SAMPLES.get(message.sample)
    if dtype is None:
        raise ValueError(f"unknown sample code {message.sample}")
    shape = (message.height, message.width) + ((message.channels,) if message.channels > 1 else ())
    count = 1
    for n in shape:
        count *= n
    image = np.frombuffer(message.payload, dtype=dtype, count=count).reshape(shape)
    return image, count * image.itemsize


def as_mask(np, name, array, shape):
    array = np.asarray(array)
    if array.shape != shape:
        raise ValueError(f"raster {name!r} has shape {array.shape}, expected {shape}")
    if array.dtype == np.bool_:
        return np.where(array, 255, 0).astype(np.uint8)
    if array.dtype != np.uint8:
        raise ValueError(f"raster {name!r} must be bool or uint8, not {array.dtype}")
    return np.ascontiguousarray(array)


def run_prepare(np, channel, module, message):
    if not hasattr(module, "prepare"):
        raise AttributeError("the prepare script defines no prepare(image, ctx)")
    image, _ = image_from(np, message)
    ctx = types.SimpleNamespace(mpp=message.value, params=dict(parse_fields(message.text)))
    answer = module.prepare(image, ctx)
    rasters, params = answer if answer is not None else ({}, {})
    rasters = rasters or {}
    params = params or {}
    shape = image.shape[:2]
    fields = [("param:" + str(k), v) for k, v in params.items()]
    payload = []
    for name, array in rasters.items():
        name = str(name)
        if not name or ":" in name:
            raise ValueError(f"raster name {name!r} is empty or contains ':'")
        fields.append(("raster", name))
        payload.append(as_mask(np, name, array, shape).tobytes())
    channel.send(RASTERS, format_fields(fields), b"".join(payload), width=shape[1],
                 height=shape[0], sample=SAMPLE_CODES["uint8"], channels=1)


def run_chunk(np, channel, module, message, output_dtype, params):
    image, used = image_from(np, message)
    masks = {}
    offset = used
    plane = message.width * message.height
    for key, name in parse_fields(message.text):
        if key == "mask":
            masks[name] = np.frombuffer(message.payload, dtype=np.uint8, count=plane,
                                        offset=offset).reshape(message.height, message.width)
            offset += plane
    ctx = types.SimpleNamespace(x0=message.x0, y0=message.y0, mpp=message.value,
                                halo=message.halo, params=params, masks=masks,
                                chunk_id=message.id, dtype=output_dtype)
    started = time.perf_counter()
    result = np.asarray(module.process(image, ctx))
    elapsed = time.perf_counter() - started
    if result.dtype == np.bool_ and output_dtype == "uint8":
        result = np.where(result, 255, 0).astype(np.uint8)
    if result.shape != (message.height, message.width):
        raise ValueError(f"process() returned shape {result.shape}, expected "
                         f"{(message.height, message.width)}")
    if result.dtype != np.dtype(output_dtype):
        raise ValueError(f"process() returned {result.dtype}, expected {output_dtype}")
    left, top, right, bottom = message.halo
    core = np.ascontiguousarray(result[top:message.height - bottom, left:message.width - right])
    channel.send(RESULT, payload=core.tobytes(), ident=message.id, width=core.shape[1],
                 height=core.shape[0], sample=SAMPLE_CODES[output_dtype], channels=1,
                 value=elapsed)


def serve(np, channel, mode, module):
    output_dtype = "uint8"
    params = {}
    while True:
        try:
            message = channel.receive()
        except EOFError:
            return 0
        if message.kind == SHUTDOWN:
            return 0
        try:
            if message.kind == SETUP and mode == "process":
                params = dict(parse_fields(message.text))
                output_dtype = params.pop(OUTPUT_DTYPE_KEY, "uint8")
                if output_dtype not in SAMPLE_CODES:
                    raise ValueError(f"unsupported output dtype {output_dtype}")
                if hasattr(module, "setup"):
                    module.setup(dict(params))
                channel.send(READY)
            elif message.kind == PREPARE and mode == "prepare":
                run_prepare(np, channel, module, message)
            elif message.kind == CHUNK and mode == "process":
                run_chunk(np, channel, module, message, output_dtype, params)
            else:
                raise RuntimeError(f"unexpected message kind {message.kind} in {mode} mode")
        except Exception:
            channel.send(ERROR, traceback.format_exc())


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("prepare", "process"):
        print(__doc__, file=sys.stderr)
        return 2
    mode, script = sys.argv[1], sys.argv[2]
    channel = Channel()
    try:
        import numpy as np
        with open(script, "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()
        module = load_script(script)
    except Exception:
        channel.send(ERROR, traceback.format_exc())
        return 1
    channel.send(HELLO, format_fields([
        ("python", platform.python_version()),
        ("numpy", np.__version__),
        ("mode", mode),
        ("script", os.path.basename(script)),
        ("sha256", digest),
    ]))
    return serve(np, channel, mode, module)


if __name__ == "__main__":
    sys.exit(main())
