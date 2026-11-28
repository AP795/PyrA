// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// Pvue's browser-client worker. PyrA's reader and Pvue's display rule, compiled
// to WebAssembly (pvue_client.js, built from Pvue/wasm), hold a sister copy of
// the open slide, built from the server's geometry. Each view is rendered here
// from the stored tiles the server sends unchanged - a channel composite from
// its monochrome channel tiles; tiles a render lacks are fetched, put, and the
// view rendered again until it is complete. Every pass posts a frame, so the
// page shows what is already decoded while the rest arrives.
//
// Views travel in lanes ('view', 'overview'): a newer request supersedes an
// older one of its own lane between passes. Passes of different lanes never
// overlap, so the tiles one render asked for are the tiles it takes.
//
// The page addresses the server through URL templates with {generation},
// {level} and {tile} placeholders and sends its own headers (Pvue.PvueClient).

'use strict';

const kMissingBatch = 64;
// A browser's connections per host: more fetches in flight only queue.
const kConcurrentFetches = 6;
// Each pass can reveal tiles of at most one more level; more means no progress.
const kMaxPasses = 12;
const kStatusOk = 0;
const kStatusIncomplete = 6;
// DisplayKind::Channels (Pvue/view_compose.h).
const kDisplayChannels = 3;

// The client script comes from the worker's own URL (?client=, set by
// Pvue.PvueClient) and is loaded at once, as the worker starts.
const kClientUrl = new URL(self.location.href).searchParams.get('client') || 'pvue_client.js';

let module = null;
let session = {generation: 0, urls: {}, headers: {}};
let opening = null;
const latest = {view: 0, overview: 0};
let exclusive = Promise.resolve();

function fill(template, values) {
	return template.replace(/\{(\w+)\}/g, (match, key) => key in values ? encodeURIComponent(values[key]) : match);
}

async function api(path) {
	const response = await fetch(path, {headers: session.headers});
	if (!response.ok) throw new Error(`${path.split('?')[0]}: server returned ${response.status}`);
	return response;
}

function clientError() { return module.UTF8ToString(module._pvue_client_error()); }

function withBytes(bytes, action) {
	const pointer = module._malloc(Math.max(1, bytes.length));
	module.HEAPU8.set(bytes, pointer);
	try { return action(pointer); } finally { module._free(pointer); }
}

// Runs `task` alone: a render pass with the fetches it starts.
function alone(task) {
	const run = exclusive.then(task);
	exclusive = run.catch(() => {});
	return run;
}

// Decoding runs on threads sharing WebAssembly memory, which a browser allows
// only on a cross-origin isolated page (the server sends COOP/COEP). Its thread
// workers must load the client script, not this one. The module resolves only
// once every thread has loaded; a browser that never starts them (the Claude
// app's browser pane starts none) would leave it pending, so the start is
// bounded and its outcome posted: 'started', or 'start-failed' with the reason.
const kStartTimeoutMs = 20000;
let ready;
if (self.crossOriginIsolated !== false) {
	importScripts(kClientUrl);
	ready = Promise.race([createPvueClient({mainScriptUrlOrBlob: kClientUrl}),
		new Promise((_, reject) => setTimeout(() => reject(new Error(
			`its decode threads did not start within ${kStartTimeoutMs / 1000} s`)), kStartTimeoutMs))]);
} else
	ready = Promise.reject(new Error('the page is not cross-origin isolated'));
ready.then(() => postMessage({type: 'started'}), error => postMessage({type: 'start-failed', message: error.message}));

// The slide's distinct channel composites: [{reference, planes: [r, g, b]}].
function composites() {
	const found = new Map();
	for (let plane = 0; plane < module._pvue_client_plane_count(); ++plane) {
		if (module._pvue_client_recipe(plane) !== kDisplayChannels) continue;
		const base = module._pvue_client_recipe_planes() >> 2;
		const planes = Array.from({length: module._pvue_client_recipe_count()}, (_, i) => module.HEAPU32[base + i]);
		const key = planes.join(',');
		if (!found.has(key)) found.set(key, {reference: planes[0], planes});
	}
	return [...found.values()];
}

async function open(task) {
	module = await ready;
	session = {generation: task.generation, urls: task.urls, headers: task.headers || {}};
	const geometry = new Uint8Array(await (await api(fill(session.urls.geometry,
		{generation: task.generation}))).arrayBuffer());
	const status = withBytes(geometry, pointer =>
		module._pvue_client_open(pointer, geometry.length, task.tbfMiB, task.pbfMiB, task.topology || 0, task.threads));
	if (status !== kStatusOk) throw new Error(clientError());
	return composites();
}

// A view the server renders - a plane whose codec the client lacks - fetched as
// a lossless HTJ2K frame and decoded here, or as raw RGBA if the server cannot
// encode HTJ2K. The server renders one plane, never a composite.
async function serverRender(request) {
	const lane = request.lane || 'view';
	latest[lane] = request.id;
	module = await ready;
	const params = new URLSearchParams({generation: session.generation, plane: request.plane, x: request.x,
		y: request.y, width: request.width, height: request.height, mpp: request.mpp, encoding: 'htj2k'});
	const response = await api(`${fill(session.urls.render, {generation: session.generation})}?${params}`);
	const body = new Uint8Array(await response.arrayBuffer());
	if (request.id !== latest[lane]) return;
	let width = Number(response.headers.get('X-Pyra-Width'));
	let height = Number(response.headers.get('X-Pyra-Height'));
	let pixels = body;
	const started = performance.now();
	if (response.headers.get('X-Pyra-Encoding') === 'htj2k') {
		if (withBytes(body, pointer => module._pvue_client_decode(pointer, body.length)) !== kStatusOk)
			throw new Error(clientError());
		width = module._pvue_client_width();
		height = module._pvue_client_height();
		pixels = module.HEAPU8.slice(module._pvue_client_pixels(), module._pvue_client_pixels() + width * height * 4);
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
			bodies[index] = new Uint8Array(await (await api(fill(session.urls.tile,
				{generation: session.generation, level, tile}))).arrayBuffer());
		}
	};
	await Promise.all(Array.from({length: Math.min(kConcurrentFetches, ids.length)}, fetchNext));
	ids.forEach(([level, tile], index) => {
		const body = bodies[index];
		if (withBytes(body, pointer => module._pvue_client_put(level, tile, pointer, body.length)) !== kStatusOk)
			throw new Error(clientError());
		stats.bytes += body.length;
	});
	stats.tiles += ids.length;
}

function missingTiles() {
	const count = module._pvue_client_take_missing(kMissingBatch);
	if (count < 0) throw new Error(clientError());
	const base = module._pvue_client_missing() >> 2;
	return Array.from({length: count}, (_, i) => [module.HEAPU32[base + 2 * i], module.HEAPU32[base + 2 * i + 1]]);
}

// One pass: render what is held, post it, and fetch what it lacked. True when
// the frame was complete.
async function pass(request, stats) {
	const started = performance.now();
	const status = request.display
		? module._pvue_client_render_display(request.plane, request.x, request.y, request.width, request.height, request.mpp)
		: module._pvue_client_render(request.plane, request.x, request.y, request.width, request.height, request.mpp);
	stats.ms += performance.now() - started;
	if (status !== kStatusOk && status !== kStatusIncomplete) throw new Error(clientError());
	let missing = status === kStatusIncomplete ? missingTiles() : [];
	const width = module._pvue_client_width();
	const height = module._pvue_client_height();
	const pixels = module.HEAPU8.slice(module._pvue_client_pixels(), module._pvue_client_pixels() + width * height * 4);
	const complete = status === kStatusOk;
	postMessage({type: 'frame', generation: session.generation, request, width, height, pixels: pixels.buffer,
		complete, ms: stats.ms, tiles: stats.tiles, bytes: stats.bytes}, [pixels.buffer]);
	while (missing.length) {
		await fetchTiles(missing, stats);
		missing = missingTiles();
	}
	return complete;
}

async function render(request) {
	const lane = request.lane || 'view';
	latest[lane] = request.id;
	await opening;
	const stats = {tiles: 0, bytes: 0, ms: 0};
	for (let count = 0; count < kMaxPasses && request.id === latest[lane]; ++count) {
		if (await alone(() => request.id === latest[lane] ? pass(request, stats) : true)) return;
	}
}

// The client's caches, {compressed: {bytes, cap, held, hits, misses}, decoded: {...}};
// empty before the module is ready or a slide is open.
function cacheUsage() {
	const pointer = module?._pvue_client_cache?.();
	if (!pointer) return {};
	const [bytes, cap, hits, misses, decodedBytes, decodedCap, decodedHits, decodedMisses, held, decodedHeld] =
		module.HEAPF64.slice(pointer >> 3, (pointer >> 3) + 10);
	return {compressed: {bytes, cap, held, hits, misses},
		decoded: {bytes: decodedBytes, cap: decodedCap, held: decodedHeld, hits: decodedHits, misses: decodedMisses}};
}

// A failure while rendering a plane - a tile whose codec this build lacks, even
// one only reached while storing it - is the plane's (or the composite's): the
// page shows it another way.
function failed(task, error) {
	postMessage({type: 'error', generation: session.generation, plane: task.request.plane,
		display: Boolean(task.request.display), lane: task.request.lane || 'view', message: error.message});
}

onmessage = event => {
	const task = event.data;
	if (task.type === 'open') {
		latest.view = latest.overview = 0;
		opening = open(task);
		opening.then(found => postMessage({type: 'opened', generation: task.generation, composites: found}),
			error => postMessage({type: 'error', generation: task.generation, plane: -1, message: error.message}));
	} else if (task.type === 'render') {
		render(task.request).catch(error => failed(task, error));
	} else if (task.type === 'cache') {
		postMessage({type: 'cache', generation: session.generation, ...cacheUsage()});
	} else if (task.type === 'clear') {
// Between passes: a render waits on fetches, never inside the module.
		(opening || Promise.resolve()).then(() => {
			if (module?._pvue_client_clear?.(task.caches) !== kStatusOk) throw new Error(clientError());
			postMessage({type: 'cache', generation: session.generation, ...cacheUsage()});
		}).catch(error => postMessage({type: 'clear-failed', generation: session.generation, message: error.message}));
	} else if (task.type === 'server') {
		serverRender(task.request).catch(error =>
			postMessage({type: 'error', generation: session.generation, plane: -1, message: error.message}));
	}
};
