// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// The browser client's worker: PyrA's reader compiled to WebAssembly
// (pyra_client.js, built from ViewW/wasm) holds a sister copy of the open slide,
// built from the server's geometry. Each view is rendered here from the stored
// tiles the server sends unchanged; tiles a render lacks are fetched, put, and
// the view rendered again until it is complete. Every pass posts a frame, so the
// page shows what is already decoded while the rest arrives. A newer view
// supersedes an older one between passes.

'use strict';

importScripts('pyra_client.js');

const kMissingBatch = 64;
// A browser's connections per host: more fetches in flight only queue.
const kConcurrentFetches = 6;
// Each pass can reveal tiles of at most one more level; more means no progress.
const kMaxPasses = 12;
const kStatusOk = 0;
const kStatusIncomplete = 6;

// Decoding runs on threads sharing WebAssembly memory, which a browser allows
// only on a cross-origin isolated page (the server sends COOP/COEP).
// Its thread workers must load pyra_client.js, not this script, which a module
// imported into a worker would otherwise take as its own.
const ready = self.crossOriginIsolated !== false ? createPyraClient({mainScriptUrlOrBlob: 'pyra_client.js'})
	: Promise.reject(new Error('the page is not cross-origin isolated'));
ready.catch(() => {});
let module = null;
let session = {token: '', generation: 0};
let opening = null;
let latest = 0;

async function api(path) {
	const response = await fetch(path, {headers: {'X-Pyra-Session': session.token}});
	if (!response.ok) throw new Error(`${path.split('?')[0]}: server returned ${response.status}`);
	return response;
}

function clientError() { return module.UTF8ToString(module._pyra_client_error()); }

function withBytes(bytes, action) {
	const pointer = module._malloc(Math.max(1, bytes.length));
	module.HEAPU8.set(bytes, pointer);
	try { return action(pointer); } finally { module._free(pointer); }
}

async function open(task) {
	module = await ready;
	session = {token: task.token, generation: task.generation};
	const geometry = new Uint8Array(await (await api(`/api/geometry?generation=${task.generation}`)).arrayBuffer());
	const status = withBytes(geometry, pointer =>
		module._pyra_client_open(pointer, geometry.length, task.cacheMiB, task.threads));
	if (status !== kStatusOk) throw new Error(clientError());
}

// A view the server renders - a plane whose codec the client lacks - fetched as
// a lossless HTJ2K frame and decoded here, or as raw RGBA if the server cannot
// encode HTJ2K.
async function serverRender(request) {
	latest = request.id;
	module = await ready;
	const params = new URLSearchParams({generation: session.generation, plane: request.plane, x: request.x,
		y: request.y, width: request.width, height: request.height, mpp: request.mpp, encoding: 'htj2k'});
	const response = await api(`/api/render?${params}`);
	const body = new Uint8Array(await response.arrayBuffer());
	if (request.id !== latest) return;
	let width = Number(response.headers.get('X-Pyra-Width'));
	let height = Number(response.headers.get('X-Pyra-Height'));
	let pixels = body;
	const started = performance.now();
	if (response.headers.get('X-Pyra-Encoding') === 'htj2k') {
		if (withBytes(body, pointer => module._pyra_client_decode(pointer, body.length)) !== kStatusOk)
			throw new Error(clientError());
		width = module._pyra_client_width();
		height = module._pyra_client_height();
		pixels = module.HEAPU8.slice(module._pyra_client_pixels(), module._pyra_client_pixels() + width * height * 4);
	}
	postMessage({type: 'frame', generation: session.generation, request, width, height, pixels: pixels.buffer,
		complete: true, server: true, ms: Number(response.headers.get('X-Pyra-Render-Ms')),
		decodeMs: performance.now() - started, tiles: 0, bytes: body.length}, [pixels.buffer]);
}

async function fetchTiles(ids, stats) {
	const bodies = new Array(ids.length);
	let next = 0;
	const fetchNext = async () => {
		while (next < ids.length) {
			const index = next++;
			const [level, tile] = ids[index];
			bodies[index] = new Uint8Array(await (await api(
				`/api/raw-tile?generation=${session.generation}&level=${level}&tile=${tile}`)).arrayBuffer());
		}
	};
	await Promise.all(Array.from({length: Math.min(kConcurrentFetches, ids.length)}, fetchNext));
	ids.forEach(([level, tile], index) => {
		const body = bodies[index];
		if (withBytes(body, pointer => module._pyra_client_put(level, tile, pointer, body.length)) !== kStatusOk)
			throw new Error(clientError());
		stats.bytes += body.length;
	});
	stats.tiles += ids.length;
}

function missingTiles() {
	const count = module._pyra_client_take_missing(kMissingBatch);
	if (count < 0) throw new Error(clientError());
	const base = module._pyra_client_missing() >> 2;
	return Array.from({length: count}, (_, i) => [module.HEAPU32[base + 2 * i], module.HEAPU32[base + 2 * i + 1]]);
}

async function render(request) {
	latest = request.id;
	await opening;
	const stats = {tiles: 0, bytes: 0, ms: 0};
	for (let pass = 0; pass < kMaxPasses && request.id === latest; ++pass) {
		const started = performance.now();
		const status = module._pyra_client_render(request.plane, request.x, request.y, request.width, request.height,
			request.mpp);
		stats.ms += performance.now() - started;
		if (status !== kStatusOk && status !== kStatusIncomplete) {
			postMessage({type: 'error', generation: session.generation, plane: request.plane, message: clientError()});
			return;
		}
		let missing = status === kStatusIncomplete ? missingTiles() : [];
		const width = module._pyra_client_width();
		const height = module._pyra_client_height();
		const pixels = module.HEAPU8.slice(module._pyra_client_pixels(), module._pyra_client_pixels() + width * height * 4);
		const complete = missing.length === 0;
		postMessage({type: 'frame', generation: session.generation, request, width, height, pixels: pixels.buffer,
			complete, ms: stats.ms, tiles: stats.tiles, bytes: stats.bytes}, [pixels.buffer]);
		if (complete) return;
		while (missing.length) {
			await fetchTiles(missing, stats);
			missing = missingTiles();
		}
	}
}

onmessage = event => {
	const task = event.data;
	if (task.type === 'open') {
		latest = 0;
		opening = open(task);
		opening.then(() => postMessage({type: 'opened', generation: task.generation}),
			error => postMessage({type: 'error', generation: task.generation, plane: -1, message: error.message}));
	} else if (task.type === 'render') {
// A failure while rendering a plane - a tile whose codec this build lacks, even
// one only reached while storing it - is the plane's: the server renders it.
		render(task.request).catch(error =>
			postMessage({type: 'error', generation: session.generation, plane: task.request.plane, message: error.message}));
	} else if (task.type === 'server') {
		serverRender(task.request).catch(error =>
			postMessage({type: 'error', generation: session.generation, plane: -1, message: error.message}));
	}
};
