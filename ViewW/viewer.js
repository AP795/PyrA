// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

'use strict';

const $ = id => document.getElementById(id);
const token = document.querySelector('meta[name="pyra-session"]').content;
const canvas = $('view');
const context = canvas.getContext('2d', {alpha: false});
const mini = $('minimap');
const miniContext = mini.getContext('2d');
// The toolbar's halo percentage, remembered by this browser; the server's
// --view-halo (config view_halo, limit view_halo_max) is the default.
const kHaloStorageKey = 'pyra.viewHalo';
// The last image this browser opened, with its open options and the root they
// are relative to; reopened when the page starts (rememberedImage).
const kImageStorageKey = 'pyra.openedImage';
// The cache settings this browser chose (Settings dialog): sizes and type of the
// server's and the browser client's caches.
const kCacheSettingsKey = 'pyra.cacheSettings';
// pyra_clear_cache's cache flags (PYRA_CACHE_COMPRESSED, PYRA_CACHE_DECODED).
const kClearCompressed = 1;
const kClearDecoded = 2;
const kCacheSides = ['server', 'browser'];
const kCacheFields = ['tbf', 'pbf', 'type'];
// The side panel's width in CSS pixels, dragged on its splitter and remembered
// by this browser: kSideDefaultPx until then, at least kSideMinPx. A drag stops
// at kSideMaxShare of the main area, so the image keeps the larger part; the
// stylesheet applies the same cap (viewer.css main) when the window narrows,
// without changing the width remembered.
const kSideStorageKey = 'pyra.sideWidth';
const kSideDefaultPx = 360;
const kSideMinPx = 240;
const kSideMaxShare = 0.6;
const kSideKeyStepPx = 16;
// Views are rendered at most this often while navigating, not after it stops.
const kRenderDelayMs = 35;
// The plane list's name for a slide's R, G and B channels shown in colour.
const kColourChannel = 'Colour (R, G, B)';
const state = {
	config: null, image: null, views: [], plane: null, zPlanes: [], preferColour: false, generation: 0, epoch: 0,
	x: 0, y: 0, mpp: 1, frame: null, inflight: null, overview: null, halo: 0,
	busy: false, opening: false, generating: false, analysing: false, openOptions: null, notice: null,
	pendingOpen: null, pendingOverview: null, pendingRender: null,
	drag: null, miniDrag: false, folder: '', folderRequest: 0, timer: null
};

function status(message) { $('status').textContent = message; }

function updateControls() {
	for (const id of ['plane', 'z-slider', 'fit', 'native', 'zoom-in', 'zoom-out'])
		$(id).disabled = state.opening || !state.image;
	updateAnalyseButton();
}

function isDicom(path) { return /\.dcm$/i.test(path || ''); }

// Network clients write (generate, make a gallery, analyse) only when the server allows it.
const remoteWriteRefused = 'Writing is disabled for network clients (server option --allow-remote-write)';
function writable() { return state.config?.remote_write !== false; }
function connectionLabel() { return state.config?.local === false ? 'Network connection' : 'Local connection'; }

// Enabled for an open DICOM slide when the server can run the bridge; while a
// run is going it stays enabled so the dialog can be reopened to watch it.
function updateAnalyseButton() {
	const analyse = state.config?.analyse;
	const path = state.openOptions?.path;
	const ready = writable() && Boolean(analyse?.available) && Boolean(state.image) && isDicom(path) && !state.opening;
	$('analyse').disabled = !ready && !state.analysing;
	$('analyse').textContent = state.analysing ? 'Analysing…' : 'Analyse…';
	$('analyse').title = !writable() ? remoteWriteRefused
		: !analyse?.available ? (analyse?.reason || 'Python processing is not available')
		: !state.image ? 'Open a DICOM slide first'
		: !state.openOptions ? 'Reopen this slide from the current image directory'
		: !isDicom(path) ? 'Analyse works on DICOM slides (.dcm) only'
		: 'Run Python scripts over the open DICOM slide into a new channel';
}

// After a reload the same channel and focal plane, if the slide still has it.
function matchingPlane(planes, previous) {
	return previous ? planes.find(plane => plane.channel === previous.channel && plane.z === previous.z) ?? null : null;
}

async function api(path, options = {}) {
	let response;
	try {
		response = await fetch(path, {...options, headers: {...options.headers, 'X-Pyra-Session': token}});
	} catch (_) {
		throw new Error('Cannot reach the viewer server: it may have stopped. Start it again and reload this page.');
	}
	if (!response.ok) {
		let message = `Server returned ${response.status}`;
		try { message = (await response.json()).error || message; } catch (_) { /* Keep HTTP error. */ }
		throw new Error(message);
	}
	return response;
}

function ratio() { return window.devicePixelRatio || 1; }
function dimensions() { return {width: canvas.width, height: canvas.height}; }
function selectedPlane() { return state.views.find(view => view.key === $('plane').value); }

function updateLabels() {
	if (!state.plane) return;
	$('zoom-label').textContent = `${(state.plane.mpp / state.mpp * 100).toFixed(1)}%`;
	$('resolution').textContent = `${state.mpp.toPrecision(4)} µm/px`;
}

function viewRequest() {
	const {width, height} = dimensions();
	const scale = Math.max(1, width / state.config.max_edge, height / state.config.max_edge);
	return {
		epoch: state.epoch, generation: state.generation,
		plane: state.plane.index, display: Boolean(state.plane.display), x: state.x - width * state.mpp / 2,
		y: state.y - height * state.mpp / 2, width: width * state.mpp,
		height: height * state.mpp, mpp: state.mpp * scale
	};
}

// Halo band rendered around the visible view on every side, as a percentage of
// the view's raster width, so slow panning moves over pixels already rendered.
function haloPercent(value) {
	const percent = Math.round(Number(value));
	return Number.isFinite(percent) ? Math.min(Math.max(percent, 0), state.config.view_halo_max) : state.config.view_halo;
}

function setupHalo() {
	let saved = null;
	try { saved = globalThis.localStorage?.getItem(kHaloStorageKey) ?? null; } catch (_) { /* Storage blocked. */ }
	state.halo = haloPercent(saved ?? state.config.view_halo);
	$('halo').max = state.config.view_halo_max;
	$('halo').value = state.halo;
}

let sidePx = kSideDefaultPx;

// The side panel's width for `px` asked in a main area `total` wide (Infinity:
// no cap, the stylesheet's applies).
function sideWidth(px, total) {
	const most = Math.max(kSideMinPx, Math.floor(total * kSideMaxShare));
	return Math.min(Math.max(Math.round(Number(px) || kSideDefaultPx), kSideMinPx), most);
}

// Sets the panel width (the CSS variable the layout and minimap height use);
// `save` remembers it, null forgets it and returns to the default. A width
// restored at page load is not capped by the main area, whose width may not be
// final yet.
function setSideWidth(px, save, total = $('main').getBoundingClientRect().width) {
	const main = $('main');
	const width = sideWidth(px ?? kSideDefaultPx, total);
	sidePx = width;
	main.style?.setProperty('--side-width', `${width}px`);
	$('splitter').setAttribute('aria-valuenow', String(width));
	try {
		if (save === null) globalThis.localStorage?.removeItem(kSideStorageKey);
		else if (save) globalThis.localStorage?.setItem(kSideStorageKey, String(width));
	} catch (_) { /* Storage blocked. */ }
	return width;
}

// The splitter between the image and the panel: drag it, use the arrow keys, or
// double-click it to return to the default width.
function setupSidePanel() {
	let saved = null;
	try { saved = globalThis.localStorage?.getItem(kSideStorageKey) ?? null; } catch (_) { /* Storage blocked. */ }
	setSideWidth(saved == null ? null : Number(saved), false, Infinity);
	const splitter = $('splitter');
	let dragging = null;
// The pointer's distance from the panel's edge where it grabbed the splitter,
// so the panel does not jump by it.
	let grab = 0;
	const widthAt = event => $('main').getBoundingClientRect().right - event.clientX + grab;
	splitter.addEventListener('pointerdown', event => {
		grab = 0;
		grab = sidePx - widthAt(event);	// widthAt adds the previous grab
		dragging = event.pointerId;
		splitter.setPointerCapture?.(event.pointerId);
		splitter.classList.toggle('dragging', true);
		event.preventDefault();
	});
	splitter.addEventListener('pointermove', event => {
		if (dragging === event.pointerId) setSideWidth(widthAt(event), false);
	});
	const stop = event => {
		if (dragging !== event.pointerId) return;
		dragging = null;
		splitter.classList.toggle('dragging', false);
		setSideWidth(widthAt(event), true);
	};
	splitter.addEventListener('pointerup', stop);
	splitter.addEventListener('pointercancel', stop);
	splitter.addEventListener('dblclick', () => setSideWidth(null, null));
	splitter.addEventListener('keydown', event => {
		const step = {ArrowLeft: kSideKeyStepPx, ArrowRight: -kSideKeyStepPx}[event.key];
		if (!step) return;
		setSideWidth(sidePx + step, true);
		event.preventDefault();
	});
}

// A new halo applies at once: frames of another halo no longer serve (servesView).
function haloChanged() {
	state.halo = haloPercent($('halo').value);
	$('halo').value = state.halo;
	try { globalThis.localStorage?.setItem(kHaloStorageKey, String(state.halo)); } catch (_) { /* Storage blocked. */ }
	if (state.plane) changed();
}

// Kept on every successful open rather than on page exit, which a browser does
// not reliably report; blocked storage just means nothing is remembered.
function rememberImage({path, backend, threads}) {
	const options = {path, backend, threads};
	try { globalThis.localStorage?.setItem(kImageStorageKey, JSON.stringify({root: state.config.root, options})); }
	catch (_) { /* Storage blocked. */ }
}

function forgetImage() {
	try { globalThis.localStorage?.removeItem(kImageStorageKey); } catch (_) { /* Storage blocked. */ }
}

// The image to reopen at start: null when none was remembered, it was opened
// under another root (its path is relative to it), or with OpenSlide where the
// server now has none.
function rememberedImage() {
	let saved = null;
	try { saved = JSON.parse(globalThis.localStorage?.getItem(kImageStorageKey) ?? 'null'); } catch (_) { return null; }
	const options = saved?.options;
	if (saved?.root !== state.config.root || typeof options?.path !== 'string' || !options.path) return null;
	if (options.backend === '1' && !state.config.openslide) return null;
	return options;
}

// Fills the Open form with a remembered image's options and opens it.
function restoreImage(options) {
	$('path').value = options.path;
	for (const key of ['backend', 'threads'])
		if (options[key] != null) $(key).value = options[key];
	requestOpen($('backend').value, true);
}

// The view plus its halo band, capped per axis at the raster edge limit.
function haloRequest(view) { return Pvue.haloRequest(view, state.halo, state.config.max_edge); }

// The shown complete frame or the request in flight already serves the view.
function needsRender(view) { return Pvue.needsRender(state.frame, state.inflight, view, state.halo); }

function cancelRenderTimer() {
	clearTimeout(state.timer);
	state.timer = null;
}

function changed() {
	updateLabels();
	draw();
	if (!state.plane || state.timer !== null || !needsRender(viewRequest())) return;
	state.timer = setTimeout(() => {
		state.timer = null;
		if (!state.plane || state.opening) return;
		const view = viewRequest();
		if (!needsRender(view)) return;
		const request = haloRequest(view);
		state.inflight = request;
		if (client?.usable(request)) { client.send(request); return; }
		if (client?.decodes(request)) { client.send(request, 'server'); return; }
		state.pendingRender = request;
		pump();
	}, kRenderDelayMs);
}

// Browser rendering (Pvue, pvue_worker.js): the worker holds a WebAssembly copy
// of PyrA's reader, fetches the stored tiles a view needs and renders it here,
// composing a slide's R, G and B channels from their own tiles. The server's
// /api/render remains the path when the build has no client, before it is
// ready, and for a plane whose codec the client lacks; it renders one plane,
// never the colour composite.
let client = null;

// Cache usage, refreshed every kCacheRefreshMs while a slide is open and the page
// visible: the server's reader (/api/cache) and the browser client's.
const kCacheRefreshMs = 1000;
const kBytesPerMiB = 1024 * 1024;
let cacheRequest = null;

const count = value => Number(value).toLocaleString('en-US');
const mebibytes = bytes => `${(bytes / kBytesPerMiB).toFixed(1)} MiB`;

function sentText(tiles) {
	return tiles ? `${count(tiles.served)} tiles · ${mebibytes(tiles.served_bytes)}` : '—';
}

// A cache's hit ratio over its tile lookups since the slide was opened, rounded
// down so that a cache that missed never shows 100%; null without lookups or
// where the backend does not count them.
function hitRatio(usage) {
	const lookups = (usage?.hits ?? 0) + (usage?.misses ?? 0);
	if (usage?.hits == null || !lookups) return null;
	return `${(Math.floor(1000 * usage.hits / lookups) / 10).toFixed(1)}% hits`;
}

function cacheText(usage) {
	if (!usage) return '—';
	if (!usage.cap) return 'off';
	const mib = bytes => (bytes / kBytesPerMiB).toFixed(bytes < 10 * kBytesPerMiB ? 1 : 0);
	const ratio = hitRatio(usage);
	const held = usage.held == null ? '' : ` · ${count(usage.held)} tiles`;
	return `${mib(usage.bytes)} / ${mib(usage.cap)} MiB${held} · ${Math.round(100 * usage.bytes / usage.cap)}% full` +
		(ratio ? ` · ${ratio}` : '');
}

// A cache's tooltip: the bytes per tile held and the lookups behind its hit
// ratio. A compressed tile is stored at its own size, so its figure is the
// tiles' average; a decoded tile takes a slot of the level's tile size.
function cacheTitle(usage, compressed) {
	if (!usage?.cap) return '';
	const parts = [];
	if (usage.held > 0) {
		const kib = (usage.bytes / usage.held / 1024).toFixed(1);
		parts.push(compressed ? `${count(usage.held)} tiles of ${kib} KiB on average, each stored at its own size.`
			: `${count(usage.held)} tiles in slots of ${kib} KiB on average, a slot per decoded tile.`);
	}
	if (usage.hits != null) parts.push(`${count(usage.hits)} hits, ${count(usage.misses)} misses since the image was opened.`);
	return parts.join(' ');
}

// Why the browser rows hold no figures: no client, still starting, or failed.
function clientCacheReason() {
	if (!client) return 'no browser client';
	if (client.startFailed) return 'not running';
	if (!client.started) return 'starting…';
	return '—';
}

// A compressed cache that sent tiles past itself (no slot to load them into)
// is too small for the workflow: the count is shown, and marked.
function showCache(owner, usage) {
	const empty = owner === 'client' && !usage?.compressed ? clientCacheReason() : '—';
	const compressed = $(`cache-${owner}-compressed`);
	const bypassed = usage?.compressed?.bypassed || 0;
	compressed.textContent = usage?.compressed ? cacheText(usage.compressed) + (bypassed ? ` · ${bypassed} bypassed` : '') : empty;
	compressed.classList.toggle('bypassed', bypassed > 0);
	compressed.title = [cacheTitle(usage?.compressed, true), bypassed ? `${bypassed} tiles were sent past this cache: ` +
		'no slot was free to hold them. Open the slide with a larger TBF.' : ''].filter(Boolean).join(' ');
	const decoded = $(`cache-${owner}-decoded`);
	decoded.textContent = usage?.decoded ? cacheText(usage.decoded) : empty;
	decoded.title = cacheTitle(usage?.decoded, false);
// The server's tile counts for the open image: read from the file, and sent.
	if (owner === 'server') {
		$('tiles-read').textContent = usage?.tiles?.read == null ? '—' : count(usage.tiles.read);
		$('transfer').textContent = sentText(usage?.tiles);
	}
}

async function refreshCache() {
	if (!state.image || cacheRequest || document.hidden) return;
	cacheRequest = api('/api/cache').then(response => response.json()).then(usage => {
		if (!usage.busy) showCache('server', usage);
	}).catch(() => {}).finally(() => { cacheRequest = null; });
	if (client?.ready) client.cacheUsage();
	else showCache('client', null);
	await cacheRequest;
}

// The server's default decoded-tile cache (PBF) for a reader: none for the native
// reader, which only sends stored tiles (kServerDecodedCacheMiB); OpenSlide has
// none to send, so the server renders and keeps the reader's default.
function defaultPbf(backend) {
	return backend === '0' ? state.config.server_pbf_mib : state.config.pbf_mib;
}

// The defaults of each setting; the server's PBF follows the reader.
function cacheDefaults(backend) {
	return {server: {tbf: state.config.tbf_mib, pbf: defaultPbf(backend), type: 0},
		browser: {tbf: state.config.client_tbf_mib, pbf: state.config.client_pbf_mib, type: 0}};
}

// The settings this browser keeps, {server, browser} each {tbf, pbf, type}: a
// value, or null for the default (so a default server PBF keeps following the
// reader).
function savedCacheSettings() {
	let saved = null;
	try { saved = JSON.parse(globalThis.localStorage?.getItem(kCacheSettingsKey) ?? 'null'); } catch (_) { /* Kept none. */ }
	const side = value => Object.fromEntries(kCacheFields.map(key =>
		[key, Number.isFinite(value?.[key]) ? value[key] : null]));
	return {server: side(saved?.server), browser: side(saved?.browser)};
}

// The server's cache options for an open with `backend`, as /api/open fields.
// OpenSlide uses only the PBF; the server ignores the others for it.
function serverCacheOptions(backend, settings = savedCacheSettings()) {
	const defaults = cacheDefaults(backend).server;
	const value = key => String(settings.server[key] ?? defaults[key]);
	return {tbf_mib: value('tbf'), pbf_mib: value('pbf'), topology: value('type')};
}

// The browser client's cache options (PvueClient's).
function browserCacheOptions(settings = savedCacheSettings()) {
	const defaults = cacheDefaults('0').browser;
	const value = key => settings.browser[key] ?? defaults[key];
	return {tbfMiB: value('tbf'), pbfMiB: value('pbf'), topology: value('type')};
}

// The reader of the open image, else the one the Open form would use.
function currentBackend() {
	return state.openOptions?.backend ?? $('backend').value;
}

const settingId = (side, key) => `settings-${side}-${key}`;

// Fills the dialog with the settings in force. Server sizes other than the PBF,
// its type and its resets are the native reader's; the browser column needs a
// running client; resets need an open image.
function openSettings() {
	const backend = currentBackend();
	const server = serverCacheOptions(backend);
	const browser = browserCacheOptions();
	const shown = {server: {tbf: server.tbf_mib, pbf: server.pbf_mib, type: server.topology},
		browser: {tbf: browser.tbfMiB, pbf: browser.pbfMiB, type: browser.topology}};
	const openslide = backend === '1';
	const clientRuns = Boolean(client) && !client.startFailed;
	for (const side of kCacheSides) {
		for (const key of kCacheFields) $(settingId(side, key)).value = String(shown[side][key]);
		for (const key of ['tbf', 'pbf']) {
			$(settingId('browser', key)).max = state.config.client_cache_max_mib;
			const reset = $(settingId(side, `reset-${key}`));
			reset.checked = false;
			reset.disabled = !state.image || (side === 'server' ? openslide : !client?.ready);
		}
	}
	for (const id of ['tbf', 'type', 'reset-tbf', 'reset-pbf']) {
		const field = $(settingId('server', id));
		if (openslide) field.disabled = true;
		else if (!id.startsWith('reset')) field.disabled = false;
		field.title = openslide ? 'OpenSlide keeps its own cache' : '';
	}
	for (const key of kCacheFields) {
		$(settingId('browser', key)).disabled = !clientRuns;
		$(settingId('browser', key)).title = clientRuns ? '' : clientCacheReason();
	}
	$('settings-note').textContent = state.image ? '' : 'No image is open: the settings apply to the next one.';
	$('settings-error').textContent = '';
	$('settings-dialog').showModal();
}

// Keeps the dialog's settings. A new size or type reopens the open image (every
// cache then starts empty, so resets are moot); otherwise the checked caches are
// reset in place.
async function applySettings() {
	const backend = currentBackend();
	const before = {server: serverCacheOptions(backend), browser: browserCacheOptions()};
	const defaults = cacheDefaults(backend);
	const settings = {};
	for (const side of kCacheSides) {
		settings[side] = {};
		for (const key of kCacheFields) {
			const value = Number($(settingId(side, key)).value);
			settings[side][key] = !Number.isFinite(value) || value === defaults[side][key] ? null : value;
		}
	}
	try { globalThis.localStorage?.setItem(kCacheSettingsKey, JSON.stringify(settings)); } catch (_) { /* Storage blocked. */ }
	const after = {server: serverCacheOptions(backend, settings), browser: browserCacheOptions(settings)};
	if (client) Object.assign(client.options, after.browser);
	const flags = side => ($(settingId(side, 'reset-tbf')).checked ? kClearCompressed : 0) |
		($(settingId(side, 'reset-pbf')).checked ? kClearDecoded : 0);
	const resets = {server: flags('server'), browser: flags('browser')};
	$('settings-dialog').close();
	if (JSON.stringify(before) !== JSON.stringify(after) && state.openOptions) {
		state.openOptions = {...state.openOptions, ...after.server};
		reloadImage('Cache settings applied; the image was reopened with empty caches.');
		return;
	}
	try {
		if (resets.server && state.image) {
			const fields = {};
			if (resets.server & kClearCompressed) fields.compressed = '1';
			if (resets.server & kClearDecoded) fields.decoded = '1';
			await api('/api/cache/clear', {method: 'POST', body: new URLSearchParams(fields)});
		}
		if (resets.browser) client?.clear(resets.browser);
		if (resets.server || resets.browser) status('Caches reset; their hit counts restart.');
	} catch (error) { status(`Caches not reset: ${error.message}`); }
	await refreshCache();
}

// The client starts with the page, so the Open form knows whether it runs
// before a slide is opened.
function makeClient() {
	if (!state.config?.client || !Pvue.PvueClient.available()) return null;
	client ??= new Pvue.PvueClient({workerUrl: 'pvue_worker.js', clientUrl: 'pvue_client.js',
		urls: {geometry: '/api/geometry?generation={generation}',
			tile: '/api/raw-tile?generation={generation}&level={level}&tile={tile}', render: '/api/render'},
		headers: {'X-Pyra-Session': token}, ...browserCacheOptions(),
		threads: navigator.hardwareConcurrency || 1,
		onOpened: clientOpened, onError: clientError, onFrame: frame => clientFrame(frame).catch(() => {}),
		onCache: usage => showCache('client', usage), onStarted: () => showCache('client', null),
		onStartFailed: clientStartFailed, onClearFailed: reason => status(`Browser caches not reset: ${reason}`)});
	client.start();
	return client;
}

function clientStartFailed(reason) {
	status(`Browser rendering unavailable (${reason}); the server renders instead.`);
	showCache('client', null);
}

function startClient() {
	makeClient()?.open(state.generation);
}

// The client's composites become the first entries of the plane list; the
// colour view is selected unless the browser chose a plane meanwhile.
function clientOpened(composites) {
	if (!state.image) return;
	const colour = composites.map(composite => {
		const reference = state.image.planes[composite.reference];
		return reference && {...reference, key: `display:${composite.reference}`, display: true, channel: kColourChannel};
	}).filter(Boolean);
	state.views = [...colour, ...state.image.planes];
	fillPlaneList();
	const choice = state.preferColour && (colour.find(view => view.z === state.plane?.z) || colour[0]);
	state.preferColour = false;
	$('plane').value = (choice || state.plane)?.key ?? $('plane').value;
	selectPlane(true);
}

function clientError(message) {
	state.inflight = null;
	if (state.plane?.display && (message.display || message.plane < 0)) {
		status(`Colour view unavailable (${message.message}); showing the plane.`);
		$('plane').value = String(state.plane.index);
		selectPlane(true);
		return;
	}
	status(`Browser rendering unavailable (${message.message}); the server renders instead.`);
// The minimap's overview is asked again, now of the server, or it stays black.
	if (message.lane === 'overview' || message.plane < 0) requestOverview();
	changed();
}

async function clientFrame(message) {
	const request = message.request;
// Any frame of this plane newer than the one shown helps: draw() places a frame
// by its own rectangle, so one from a slightly older view still lands right.
	if (request.epoch !== state.epoch) return;
	const bitmap = await createImageBitmap(new ImageData(new Uint8ClampedArray(message.pixels), message.width,
		message.height), {colorSpaceConversion: 'none'});
	if (request.epoch !== state.epoch || request.id < client.shown[request.lane]) { bitmap.close(); return; }
	if (request.lane === 'overview') {
		state.overview?.close();
		state.overview = bitmap;
		drawMini();
		return;
	}
	state.frame?.bitmap.close();
	state.frame = {bitmap, request, complete: message.complete};
	draw();
	$('render-time').textContent = `${message.ms.toFixed(2)} ms (${message.server ? 'server' : 'browser'})`;
	$('total-time').textContent = `${(performance.now() - request.started).toFixed(2)} ms`;
	$('raster').textContent = `${message.width} × ${message.height}`;
	status(message.complete ? state.notice || 'Ready · Drag to pan, scroll to zoom' : 'Loading tiles…');
}

function fit() {
	if (!state.plane || state.opening) return;
	const plane = state.plane;
	state.x = plane.x + plane.width / 2;
	state.y = plane.y + plane.height / 2;
	state.mpp = Math.max(plane.width / canvas.width, plane.height / canvas.height);
	changed();
}

function zoom(factor, cssX = canvas.clientWidth / 2, cssY = canvas.clientHeight / 2) {
	if (!state.plane || state.opening) return;
	const dx = (cssX - canvas.clientWidth / 2) * ratio();
	const dy = (cssY - canvas.clientHeight / 2) * ratio();
	const fitMpp = Math.max(state.plane.width / canvas.width, state.plane.height / canvas.height);
	const next = Math.min(Math.max(state.mpp * factor, state.plane.mpp / 64), Math.max(fitMpp, state.plane.mpp) * 64);
	state.x += dx * (state.mpp - next);
	state.y += dy * (state.mpp - next);
	state.mpp = next;
	changed();
}

function nativeSize() {
	if (!state.plane || state.opening) return;
	state.mpp = state.plane.mpp;
	changed();
}

function miniBox() {
	if (!state.plane) return null;
	const scale = Math.min(mini.width / state.plane.width, mini.height / state.plane.height);
	const width = state.plane.width * scale;
	const height = state.plane.height * scale;
	return {x: (mini.width - width) / 2, y: (mini.height - height) / 2, width, height, scale};
}

function drawMini() {
	miniContext.clearRect(0, 0, mini.width, mini.height);
	const box = miniBox();
	if (!box) return;
	if (state.overview) miniContext.drawImage(state.overview, box.x, box.y, box.width, box.height);
	miniContext.save();
	miniContext.beginPath();
	miniContext.rect(box.x, box.y, box.width, box.height);
	miniContext.clip();
	miniContext.fillStyle = '#a6e3ca20';
	const x = box.x + (state.x - canvas.width * state.mpp / 2 - state.plane.x) * box.scale;
	const y = box.y + (state.y - canvas.height * state.mpp / 2 - state.plane.y) * box.scale;
	const width = canvas.width * state.mpp * box.scale;
	const height = canvas.height * state.mpp * box.scale;
	miniContext.fillRect(x, y, width, height);
	const dpr = ratio();
	miniContext.lineWidth = 4 * dpr;
	miniContext.strokeStyle = '#111';
	miniContext.strokeRect(x, y, width, height);
	miniContext.lineWidth = 2 * dpr;
	miniContext.strokeStyle = '#fff';
	miniContext.strokeRect(x, y, width, height);
	miniContext.restore();
}

function draw() {
	context.fillStyle = '#11171c';
	context.fillRect(0, 0, canvas.width, canvas.height);
	if (state.frame && state.plane) {
		const frame = state.frame;
		const left = state.x - canvas.width * state.mpp / 2;
		const top = state.y - canvas.height * state.mpp / 2;
		context.imageSmoothingEnabled = state.mpp > state.plane.mpp;
		context.drawImage(frame.bitmap, (frame.request.x - left) / state.mpp,
			(frame.request.y - top) / state.mpp, frame.request.width / state.mpp, frame.request.height / state.mpp);
	}
	drawMini();
}

function resize() {
	const dpr = ratio();
	for (const item of [canvas, mini]) {
		const width = Math.max(1, Math.round(item.clientWidth * dpr));
		const height = Math.max(1, Math.round(item.clientHeight * dpr));
		if (item.width !== width || item.height !== height) { item.width = width; item.height = height; }
	}
	if (state.plane) changed(); else draw();
}

function updateZSlider() {
	state.zPlanes = state.views.filter(plane => plane.channel === state.plane.channel)
		.sort((a, b) => a.z - b.z);
	const slider = $('z-slider');
	slider.max = state.zPlanes.length - 1;
	slider.value = state.zPlanes.findIndex(plane => plane.key === state.plane.key);
	const label = state.plane.z_is_index ? `Z plane ${state.plane.z + 1}` : `Z ${state.plane.z} µm`;
	$('z-position').textContent = label;
	slider.setAttribute('aria-valuetext', label);
	$('z-control').hidden = state.zPlanes.length < 2;
}

function selectPlane(preserveView = false) {
	state.plane = selectedPlane();
	updateZSlider();
	++state.epoch;
	state.pendingRender = state.inflight = null;
	state.frame?.bitmap.close();
	state.overview?.close();
	state.frame = state.overview = null;
	requestOverview();
	if (preserveView) changed(); else fit();
	pump();
}

// The minimap's overview: rendered by the browser client from the coarsest
// tiles, the colour composite included; the server renders it otherwise.
function requestOverview() {
	const plane = state.plane;
	const request = {epoch: state.epoch, generation: state.generation, plane: plane.index, display: Boolean(plane.display),
		x: plane.x, y: plane.y, width: plane.width, height: plane.height,
		mpp: Math.max(plane.width, plane.height) / state.config.overview_edge};
	if (client?.usable(request)) client.send(request, 'render', 'overview');
	else state.pendingOverview = {generation: state.generation, plane: plane.index, overview: 1, epoch: state.epoch};
}

// The plane list: the colour composites first, then every plane.
function fillPlaneList() {
	$('plane').replaceChildren(...state.views.map(plane => {
		const option = document.createElement('option');
		option.value = plane.key;
		option.textContent = `${plane.channel || 'Default'} · ${plane.z_is_index ? `Z plane ${plane.z + 1}` : `Z ${plane.z} µm`}`;
		return option;
	}));
}

async function pixels(request) {
	const params = new URLSearchParams(Object.entries(request).filter(([key]) => key !== 'epoch' && key !== 'halo'));
	const started = performance.now();
	const response = await api(`/api/render?${params}`);
	const width = Number(response.headers.get('X-Pyra-Width'));
	const height = Number(response.headers.get('X-Pyra-Height'));
	const bytes = await response.arrayBuffer();
	if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 ||
		width > state.config.max_edge || height > state.config.max_edge || bytes.byteLength !== width * height * 4)
		throw new Error('Server returned an invalid frame');
	const bitmap = await createImageBitmap(new ImageData(new Uint8ClampedArray(bytes), width, height), {colorSpaceConversion: 'none'});
	return {bitmap, width, height, started, bytes: bytes.byteLength,
		nativeMs: Number(response.headers.get('X-Pyra-Render-Ms')),
		incomplete: response.headers.get('X-Pyra-Incomplete') === '1'};
}

async function doOpen(task) {
	const response = await api('/api/open', {method: 'POST', body: new URLSearchParams(task.options)});
	const image = await response.json();
	if (task.epoch !== state.epoch) return;
	for (const plane of image.planes) plane.key = String(plane.index);
	const previous = state.plane;
	state.image = image;
	state.views = [...image.planes];
	state.generation = image.generation;
	startClient();
	fillPlaneList();
	const defaultIndex = Number(image.properties.find(([key]) => key === 'pyra.default-plane-index')?.[1] ?? 0);
	const kept = task.reload ? matchingPlane(image.planes, previous?.display ? image.planes[previous.index] : previous) : null;
// A first open shows the colour view when the client finds one; a reload keeps
// the kind of view that was shown.
	state.preferColour = task.reload ? Boolean(previous?.display) : true;
	if (kept) $('plane').value = kept.key;
	else if (image.planes.some(plane => plane.index === defaultIndex)) $('plane').value = String(defaultIndex);
	state.openOptions = {...task.options};
	rememberImage(state.openOptions);
	$('metadata').replaceChildren(...image.properties.map(([key, value]) => {
		const row = document.createElement('div'); row.className = 'property';
		const name = document.createElement('div'); name.className = 'property-name'; name.textContent = key;
		const text = document.createElement('div'); text.className = 'property-value'; text.textContent = value;
		row.append(name, text); return row;
	}));
	$('welcome').hidden = true;
	$('image-label').hidden = false;
	$('image-label').textContent = task.options.path;
	$('open-dialog').close();
	status(state.notice || `Opened ${task.options.path} · ${image.open_ms.toFixed(1)} ms`);
	state.opening = false;
	updateControls();
	selectPlane(Boolean(kept));
}

async function pump() {
	if (state.busy) return;
	let kind, task;
	if (state.pendingOpen) { kind = 'open'; task = state.pendingOpen; state.pendingOpen = null; }
	else if (state.pendingOverview) { kind = 'overview'; task = state.pendingOverview; state.pendingOverview = null; }
	else if (state.pendingRender) { kind = 'view'; task = state.pendingRender; state.pendingRender = null; }
	else return;
	state.busy = true;
	$('connection').textContent = kind === 'open' ? 'Opening…' : 'Rendering…';
	try {
		if (kind === 'open') await doOpen(task);
		else {
			const frame = await pixels(task);
// A view frame stays useful after the view moved on: draw() places it by its
// own rectangle, and a newer view is already scheduled if it no longer serves.
			if (task.epoch !== state.epoch) frame.bitmap.close();
			else if (kind === 'overview') {
				state.overview?.close(); state.overview = frame.bitmap; drawMini();
			} else {
				state.frame?.bitmap.close();
				state.frame = {bitmap: frame.bitmap, request: task, complete: !frame.incomplete};
				draw();
				$('render-time').textContent = `${frame.nativeMs.toFixed(2)} ms`;
				$('total-time').textContent = `${(performance.now() - frame.started).toFixed(2)} ms`;
				$('raster').textContent = `${frame.width} × ${frame.height}`;
				status(frame.incomplete ? 'Incomplete render: unavailable image areas are black.' : state.notice || 'Ready · Drag to pan, scroll to zoom');
			}
		}
	} catch (error) {
		if (kind === 'view' && state.inflight === task) state.inflight = null;
		if (task.epoch === state.epoch) {
			status(error.message);
			if (kind === 'open') {
// A remembered image that no longer opens is not tried at every start.
				if (task.restored) forgetImage();
				$('open-error').textContent = error.message;
				state.opening = false;
				updateControls();
				if (state.plane) changed();
			}
		}
	} finally {
		state.busy = false;
		$('open-submit').disabled = state.opening;
		$('connection').textContent = connectionLabel();
		pump();
	}
}

function requestOpen(backend = $('backend').value, restored = false) {
	if (state.opening) return;
	state.notice = null;
	state.opening = true;
	updateControls();
	state.pendingOverview = state.pendingRender = null;
	cancelRenderTimer();
	++state.epoch;
	state.pendingOpen = {epoch: state.epoch, restored, options: {
		path: $('path').value, backend, threads: $('threads').value, ...serverCacheOptions(backend)
	}};
	$('open-error').textContent = '';
	$('open-submit').disabled = true;
	pump();
}

// Reopens the current slide with the options it was opened with, keeping the
// view and the plane: an analysis has added channels to it.
function reloadImage(message) {
	if (!state.openOptions || state.opening) return;
	state.notice = message;
	state.opening = true;
	updateControls();
	state.pendingOverview = state.pendingRender = null;
	cancelRenderTimer();
	++state.epoch;
	state.pendingOpen = {epoch: state.epoch, options: {...state.openOptions}, reload: true};
	pump();
}

function relative(folder, name) { return [folder === '.' ? '' : folder, name].filter(Boolean).join('/'); }

async function showFolder(folder) {
	const sequence = ++state.folderRequest;
	try {
		const listing = await (await api(`/api/files?${new URLSearchParams({path: folder})}`)).json();
		if (sequence !== state.folderRequest) return;
		state.folder = listing.path === '.' ? '' : listing.path;
		$('folder-label').textContent = `/${state.folder}`;
		$('parent-folder').disabled = !state.folder;
		$('file-list').replaceChildren(...listing.entries.map(entry => {
			const button = document.createElement('button'); button.type = 'button';
			button.className = `file-entry${entry.directory ? ' folder' : ''}`;
			button.textContent = `${entry.directory ? '▸ ' : ''}${entry.name}`;
			button.addEventListener('click', () => {
				const path = relative(state.folder, entry.name);
				if (entry.directory && !entry.name.toLowerCase().endsWith('.glr')) showFolder(path);
				else {
					$('path').value = path;
					for (const child of $('file-list').children) child.classList.remove('selected');
					button.classList.add('selected');
				}
			});
			return button;
		}));
		if (listing.truncated) $('open-error').textContent = 'Showing the first 512 entries. You can also enter a relative path.';
	} catch (error) {
		$('open-error').textContent = error.message;
		$('open-error').scrollIntoView?.({block: 'nearest'});
	}
}

function showOpen() {
	if (!state.config) return;
	if (!$('open-dialog').open) $('open-dialog').showModal();
	$('open-error').textContent = '';
	showFolder(state.folder);
}

for (const id of ['browse', 'welcome-open']) $(id).addEventListener('click', showOpen);
for (const id of ['close-dialog', 'cancel-dialog']) $(id).addEventListener('click', () => $('open-dialog').close());
$('settings').addEventListener('click', openSettings);
for (const id of ['settings-close', 'settings-cancel']) $(id).addEventListener('click', () => $('settings-dialog').close());
$('settings-form').addEventListener('submit', event => { event.preventDefault(); applySettings(); });
$('open-form').addEventListener('submit', event => { event.preventDefault(); requestOpen(); });
$('parent-folder').addEventListener('click', () => showFolder(state.folder.split('/').slice(0, -1).join('/')));

// The server's image directory. A browser on this machine may move it for every
// session; the server keeps it for its next start without --root. Network
// clients may not, since it is what confines them. An open slide beneath the new
// directory keeps its path; one outside stays shown but cannot be reloaded.
function showRootEditor(show) {
	$('root-editor').hidden = !show;
	$('change-root').hidden = show || state.config?.local !== true;
	if (show) { $('root-input').value = state.config.root; $('root-input').focus?.(); }
}

async function setRoot() {
	$('open-error').textContent = '';
	try {
		const changed = await (await api(`/api/root?${new URLSearchParams({path: $('root-input').value.trim()})}`,
			{method: 'POST'})).json();
		state.config.root = changed.root;
		$('root-label').textContent = changed.root;
		if (state.openOptions) {
			if (changed.slide === null) state.openOptions = null;
			else state.openOptions.path = changed.slide;
		}
		updateAnalyseButton();
		showRootEditor(false);
		$('path').value = '';
		await showFolder('');
		if (!changed.saved) $('open-error').textContent = `Changed, but not kept for the next start: ${changed.save_error}`;
	} catch (error) {
		$('open-error').textContent = error.message;
		$('open-error').scrollIntoView?.({block: 'nearest'});
	}
}

$('change-root').addEventListener('click', () => showRootEditor(true));
$('cancel-root').addEventListener('click', () => showRootEditor(false));
$('set-root').addEventListener('click', setRoot);
// Enter in the directory field sets it rather than submitting the Open form.
$('root-input').addEventListener('keydown', event => {
	if (event.key === 'Enter') { event.preventDefault(); setRoot(); }
	else if (event.key === 'Escape') { event.preventDefault(); showRootEditor(false); }
});
$('make-gallery').addEventListener('click', async () => {
	$('open-error').textContent = '';
	try {
		let params = {path: state.folder};
		let response = await fetch(`/api/make-gallery?${new URLSearchParams(params)}`, {method: 'POST', headers: {'X-Pyra-Session': token}});
		if (response.status === 400) {
			const message = (await response.json()).error || '';
			if (/pixel size|microns per pixel/i.test(message)) {
				const mpp = prompt('No image states its pixel size. Microns per pixel:', '1');
				if (!mpp) return;
				params = {...params, mpp};
				response = await fetch(`/api/make-gallery?${new URLSearchParams(params)}`, {method: 'POST', headers: {'X-Pyra-Session': token}});
			} else throw new Error(message);
		}
		if (!response.ok) throw new Error((await response.json()).error || `Server returned ${response.status}`);
		const made = await response.json();
		await showFolder(state.folder);
		$('path').value = made.path;
	} catch (error) { $('open-error').textContent = error.message; }
});
// Real-valued generator fields: element, request parameter, config key, and
// display scale (fractions are edited as percentages).
const generateReals = [
	['generate-z-step', 'z_step', 'z_step', 1], ['generate-focus', 'focus', 'focus', 100],
	['generate-nucleus', 'nucleus', 'nucleus', 1], ['generate-spacing', 'spacing', 'spacing', 1],
	['generate-positive', 'positive', 'positive', 100], ['generate-tumour', 'tumour', 'tumour', 100],
	['generate-lymphocytes', 'lymphocytes', 'lymphocytes', 100]];
const generateFields = ['generate-format', 'generate-name', 'generate-width', 'generate-height', 'generate-tile', 'generate-z',
	'generate-mpp', 'generate-codec', 'generate-quality', 'generate-lossless', ...generateReals.map(field => field[0])];

function setupGenerate() {
	const synthetic = state.config.synthetic;
	for (const [id, value, min, max, step] of [
		['generate-width', synthetic.width, 1, synthetic.edge_max, 1],
		['generate-height', synthetic.height, 1, synthetic.edge_max, 1],
		['generate-tile', synthetic.tile, 0, synthetic.tile_max, synthetic.tile_step],
		['generate-z', synthetic.z, 1, synthetic.z_max, 1],
		['generate-mpp', synthetic.mpp, synthetic.mpp_min, synthetic.mpp_max, 0.001],
		['generate-z-step', synthetic.z_step, synthetic.z_step_min, synthetic.z_step_max, 0.05],
		['generate-focus', +(synthetic.focus * 100).toFixed(2), 0, 100, 0.01],
		['generate-nucleus', synthetic.nucleus, synthetic.nucleus_min, synthetic.nucleus_max, 0.1],
		['generate-spacing', synthetic.spacing, synthetic.spacing_min, synthetic.spacing_max, 0.1],
		['generate-positive', synthetic.positive * 100, 0, 100, 0.1],
		['generate-tumour', synthetic.tumour * 100, 0, 100, 0.1],
		['generate-lymphocytes', synthetic.lymphocytes * 100, 0, 100, 0.1]]) {
		Object.assign($(id), {value, min, max, step});
	}
	$('generate-quality').value = synthetic.quality;
	$('generate-dicom').disabled = !synthetic.codecs.some(codec => codec.dicom);
	$('generate-dicom').title = $('generate-dicom').disabled ? 'DICOM generation needs a build with DCMTK' : '';
	$('generate-format').value = 'ptf';
	state.generateTile = synthetic.tile;
	updateGenerateFormat(synthetic.codec);
}

function generateUntiled() { return Number($('generate-tile').value) === 0; }
function isUntiledImage(path) { return /\.tiff?$/i.test(path || ''); }

// Tile edges between 0 (untiled) and the minimum do not exist: stepping down from
// the minimum lands on 0, stepping up from 0 on the minimum.
function snapGenerateTile() {
	const tile = $('generate-tile');
	const minimum = state.config.synthetic.tile_min;
	const value = Number(tile.value);
	if (value > 0 && value < minimum) tile.value = value > state.generateTile ? minimum : 0;
	state.generateTile = Number(tile.value);
	updateGenerateFormat($('generate-codec').value);
}

function updateGenerateFormat(preferredCodec) {
	const synthetic = state.config.synthetic;
	const untiled = generateUntiled();
	$('generate-dicom').disabled = untiled || !synthetic.codecs.some(codec => codec.dicom);
	if (untiled && $('generate-format').value === 'dicom') $('generate-format').value = 'ptf';
	$('generate-format').options[0].textContent = untiled ? 'TIFF (untiled)' : 'PTF';
	const dicom = $('generate-format').value === 'dicom';
	if (untiled) $('generate-z').value = 1;
	$('generate-z').disabled = state.generating || untiled;
	for (const id of ['generate-width', 'generate-height']) {
		$(id).max = untiled ? synthetic.untiled_edge_max : synthetic.edge_max;
		if (Number($(id).value) > Number($(id).max)) $(id).value = $(id).max;
	}
	const codecs = synthetic.codecs.filter(codec => dicom ? codec.dicom && codec.tiled : untiled ? codec.untiled : codec.tiled);
	$('generate-codec').replaceChildren(...codecs.map(codec => {
		const option = document.createElement('option');
		option.value = codec.id; option.textContent = codec.name;
		return option;
	}));
	const preferred = codecs.find(codec => String(codec.id) === String(preferredCodec));
	$('generate-codec').value = preferred?.id ?? codecs[0]?.id ?? '';
	const name = $('generate-name');
	if (name.value && !(untiled && /\.tiff$/i.test(name.value)))
		name.value = name.value.replace(/\.(?:ptf|dcm|tiff?)$/i, '') + (dicom ? '.dcm' : untiled ? '.tif' : '.ptf');
	updateGenerateQuality();
}

// LZW is lossless only; JPEG has no lossless mode.
function updateGenerateQuality() {
	const codec = state.config.synthetic.codecs.find(item => String(item.id) === String($('generate-codec').value));
	const canLossless = Boolean(codec?.lossless);
	const lossy = codec?.lossy !== false;
	if (!canLossless) $('generate-lossless').checked = false;
	if (!lossy) $('generate-lossless').checked = true;
	$('generate-lossless').disabled = state.generating || !canLossless || !lossy;
	$('generate-quality').disabled = state.generating || $('generate-lossless').checked;
}

function setGenerating(generating) {
	state.generating = generating;
	for (const id of generateFields) $(id).disabled = generating;
	$('generate-submit').disabled = generating;
	$('generate').disabled = !writable() || (generating && !$('generate-dialog').open);
	if (generating) updateGenerateQuality();
	else updateGenerateFormat($('generate-codec').value);
}

function showGenerate() {
	if (!state.config) return;
	if (!state.generating) {
		$('generate-error').textContent = '';
		$('generate-progress').hidden = true;
	}
	$('generate-folder').textContent = `/${state.folder}`;
	if (!$('generate-dialog').open) $('generate-dialog').showModal();
}

async function pollGenerate() {
	const progress = await (await api('/api/generate')).json();
	if (progress.total) { $('generate-progress').max = progress.total; $('generate-progress').value = progress.done; }
	const untiled = isUntiledImage(progress.path);
	status(`Generating ${progress.path} · ${progress.done} / ${progress.total || '…'} ${untiled ? 'strips' : 'tiles'}`);
	if (progress.running) { setTimeout(() => pollGenerate().catch(failGenerate), 250); return; }
	setGenerating(false);
	if (progress.error) { failGenerate(new Error(progress.error)); return; }
	$('generate-dialog').close();
	status(`Generated ${progress.path}`);
// An untiled TIFF is not a slide: it opens through a gallery (Make gallery).
	if (untiled) { await showFolder(state.folder); return; }
	$('path').value = progress.path;
	requestOpen(isDicom(progress.path) ? '0' : $('backend').value);
}

function failGenerate(error) {
	setGenerating(false);
	$('generate-error').textContent = error.message;
	status(error.message);
}

$('generate').addEventListener('click', showGenerate);
$('generate-format').addEventListener('change', () => updateGenerateFormat($('generate-codec').value));
$('generate-tile').addEventListener('change', snapGenerateTile);
$('generate-codec').addEventListener('change', updateGenerateQuality);
$('generate-lossless').addEventListener('change', updateGenerateQuality);
$('generate-close').addEventListener('click', () => $('generate-dialog').close());
$('generate-dialog').addEventListener('close', () => { $('generate').disabled = !writable() || state.generating; });
$('generate-cancel').addEventListener('click', () => {
	if (state.generating) api('/api/generate-cancel', {method: 'POST'}).catch(failGenerate);
	else $('generate-dialog').close();
});
$('generate-form').addEventListener('submit', async event => {
	event.preventDefault();
	if (state.generating) return;
	$('generate-error').textContent = '';
	const params = {
		path: state.folder, format: $('generate-format').value, name: $('generate-name').value, width: $('generate-width').value,
		height: $('generate-height').value, tile: $('generate-tile').value, z: $('generate-z').value,
		mpp: $('generate-mpp').value, codec: $('generate-codec').value, quality: $('generate-quality').value,
		lossless: $('generate-lossless').checked ? '1' : '0'
	};
	for (const [id, name, , scale] of generateReals) params[name] = String(Number($(id).value) / scale);
	setGenerating(true);
	$('generate-progress').hidden = false;
	$('generate-progress').value = 0;
	try {
		await api(`/api/generate?${new URLSearchParams(params)}`, {method: 'POST'});
		await pollGenerate();
	} catch (error) { failGenerate(error); }
});
const analyseFields = ['analyse-prepare', 'analyse-process', 'analyse-channel', 'analyse-type', 'analyse-halo',
	'analyse-workers', 'analyse-params', 'analyse-mode-cli', 'analyse-mode-server'];

function setupAnalyse() {
	const analyse = state.config.analyse;
	if (!analyse) return;
	$('analyse-workers').max = analyse.cores;
	$('analyse-workers').value = analyse.workers;
	$('analyse-halo').max = analyse.max_halo;
	$('analyse-mode-cli').disabled = !analyse.cli;
	updateAnalyseButton();
}

function setAnalysing(analysing) {
	state.analysing = analysing;
	for (const id of analyseFields) $(id).disabled = analysing;
	if (!analysing) $('analyse-mode-cli').disabled = !state.config?.analyse?.cli;
	$('analyse-submit').disabled = analysing;
	updateAnalyseButton();
}

// The last submitted choice when still offered, else the bundled example, else
// "(none)" for the optional prepare script or the first script.
function fillScripts(select, scripts, optional, chosen, example) {
	select.replaceChildren(...(optional ? [''] : []).concat(scripts).map(name => {
		const option = document.createElement('option');
		option.value = name; option.textContent = name || '(none)';
		return option;
	}));
	const offered = name => name !== undefined && (name === '' ? optional : scripts.includes(name));
	select.value = offered(chosen) ? chosen : scripts.includes(example) ? example : optional ? '' : scripts[0] ?? '';
}

async function showAnalyse() {
	if (!state.config?.analyse) return;
	$('analyse-slide').textContent = state.openOptions?.path || '';
	if (!state.analysing) {
		$('analyse-error').textContent = '';
		$('analyse-progress').hidden = true;
	}
	if (!$('analyse-dialog').open) $('analyse-dialog').showModal();
	try {
		const listing = await (await api('/api/scripts')).json();
		$('analyse-folder').textContent = listing.folder;
		fillScripts($('analyse-prepare'), listing.scripts, true, state.analyseChoice?.prepare, 'tissue_prepare.py');
		fillScripts($('analyse-process'), listing.scripts, false, state.analyseChoice?.process, 'hed_dab.py');
		if (!listing.scripts.length) $('analyse-error').textContent = 'The scripts folder holds no .py file.';
	} catch (error) { $('analyse-error').textContent = error.message; }
}

// "Analysed by server in 21.1 s with 10 Python workers: DAB, DAB.roi"
function analyseMessage(progress) {
	return `Analysed by ${progress.mode === 'cli' ? 'CLI' : 'server'} in ${Number(progress.wall_s).toFixed(1)} s ` +
		`with ${progress.workers} Python workers: ${progress.stats.channels.join(', ')}`;
}

// The result as tables: [caption, rows], each row [label, value, share?].
function analyseTables(progress) {
	const {stats} = progress;
	const seconds = value => `${value.toFixed(2)} s`;
	const share = value => stats.time.total > 0 ? `${Math.round(100 * value / stats.time.total)} %` : '';
	return [
		['Result', [
			['Channels added', stats.channels.join(', ')],
			['Chunks processed by Python', `${stats.chunks.processed} of ${stats.chunks.total}`],
			['Chunks skipped (no source frame, or roi all zero)', String(stats.chunks.skipped)],
			...(stats.chunks.mask_only ? [['… of them still written for kept masks', String(stats.chunks.mask_only)]] : []),
			['New DICOM files (one per channel and level)', String(stats.files)],
			['Frames · encoded size', `${stats.frames} · ${(stats.encoded_bytes / 1e6).toFixed(1)} MB`]]],
		['Wall time', [
			['Opening and checks', seconds(stats.time.opening ?? 0), share(stats.time.opening ?? 0)],
			['Prepare (coarse whole-slide pass)', seconds(stats.time.prepare), share(stats.time.prepare)],
			['Chunks (Python workers)', seconds(stats.time.chunks), share(stats.time.chunks)],
			['Assembly and coarser levels', seconds(stats.time.assemble), share(stats.time.assemble)],
			['Publish', seconds(stats.time.publish), share(stats.time.publish)],
			['Total in the bridge', seconds(stats.time.total), ''],
			[progress.mode === 'cli' ? 'Total seen by the viewer (CLI start-up included)' : 'Total seen by the viewer',
				seconds(Number(progress.wall_s)), '']]],
		[`Chunk work, summed over the ${stats.workers} Python workers`, [
			['Read source pixels', seconds(stats.work.read)],
			['Python', seconds(stats.work.python)],
			['Transfer to and from Python', seconds(stats.work.transfer)],
			['Encode result frames', seconds(stats.work.encode)],
			['Encode kept masks', seconds(stats.work.masks)],
			['Spool (main thread)', seconds(stats.work.spool)],
			['Workers busy on average', `${stats.busy_workers.toFixed(1)} of ${stats.workers}`]]]];
}

function showAnalyseResult(progress) {
	$('analyse-result').replaceChildren(...analyseTables(progress).map(([title, rows]) => {
		const table = document.createElement('table');
		const caption = document.createElement('caption');
		caption.textContent = title;
		table.append(caption);
		for (const cells of rows) {
			const row = document.createElement('tr');
			cells.forEach((text, index) => {
				const cell = document.createElement(index ? 'td' : 'th');
				cell.textContent = text;
				row.append(cell);
			});
			table.append(row);
		}
		return table;
	}));
	$('analyse-result').hidden = false;
}

async function pollAnalyse() {
	const progress = await (await api('/api/analyse')).json();
	if (progress.running) {
		if (progress.total) { $('analyse-progress').max = progress.total; $('analyse-progress').value = progress.done; }
		const step = `${progress.phase} ${progress.done} / ${progress.total || '…'}`;
		$('analyse-phase').textContent = step;
		status(`Analysing ${progress.path} · ${step}`);
		setTimeout(() => pollAnalyse().catch(failAnalyse), 500);
		return;
	}
	setAnalysing(false);
	$('analyse-progress').hidden = true;
	if (progress.error) { failAnalyse(new Error(progress.error)); return; }
	const message = analyseMessage(progress);
	$('analyse-phase').textContent = message;
	showAnalyseResult(progress);
	if (!$('analyse-dialog').open) $('analyse-dialog').showModal();
	if (state.openOptions?.path === progress.path) reloadImage(message);
	else status(message);
}

function failAnalyse(error) {
	setAnalysing(false);
	$('analyse-progress').hidden = true;
	$('analyse-phase').textContent = '';
	$('analyse-error').textContent = error.message;
	status(error.message.split('\n')[0]);
	if (!$('analyse-dialog').open) $('analyse-dialog').showModal();
}

$('analyse').addEventListener('click', showAnalyse);
$('analyse-close').addEventListener('click', () => $('analyse-dialog').close());
$('analyse-cancel').addEventListener('click', () => $('analyse-dialog').close());
$('analyse-form').addEventListener('submit', async event => {
	event.preventDefault();
	if (state.analysing) return;
	const params = {
		mode: $('analyse-mode-cli').checked ? 'cli' : 'server', process: $('analyse-process').value,
		prepare: $('analyse-prepare').value, channel: $('analyse-channel').value, type: $('analyse-type').value,
		halo: $('analyse-halo').value, workers: $('analyse-workers').value, params: $('analyse-params').value
	};
	state.analyseChoice = {prepare: params.prepare, process: params.process};
	$('analyse-error').textContent = '';
	$('analyse-result').hidden = true;
	$('analyse-phase').textContent = 'Starting…';
	$('analyse-progress').hidden = false;
	$('analyse-progress').value = 0;
	setAnalysing(true);
	try {
		await api('/api/analyse', {method: 'POST', body: new URLSearchParams(params)});
		await pollAnalyse();
	} catch (error) { failAnalyse(error); }
});
$('plane').addEventListener('change', () => {
	state.preferColour = false;
	selectPlane(state.plane?.channel === selectedPlane()?.channel);
});
$('z-slider').addEventListener('input', () => {
	const plane = state.zPlanes[Number($('z-slider').value)];
	if (!plane || state.opening) return;
	$('plane').value = plane.key;
	selectPlane(true);
});
$('backend').addEventListener('change', () => { $('threads').disabled = $('backend').value === '1'; });
$('fit').addEventListener('click', fit);
$('halo').addEventListener('change', haloChanged);
$('native').addEventListener('click', nativeSize);
$('zoom-in').addEventListener('click', () => zoom(1 / 1.25));
$('zoom-out').addEventListener('click', () => zoom(1.25));

canvas.addEventListener('wheel', event => {
	event.preventDefault();
	const rect = canvas.getBoundingClientRect();
	zoom(Math.exp(Math.max(-2, Math.min(2, event.deltaY * .002))), event.clientX - rect.left, event.clientY - rect.top);
}, {passive: false});
canvas.addEventListener('pointerdown', event => {
	if (!state.plane || state.opening || event.button !== 0) return;
	canvas.focus(); canvas.setPointerCapture(event.pointerId);
	state.drag = {id: event.pointerId, x: event.clientX, y: event.clientY}; canvas.classList.add('dragging');
});
canvas.addEventListener('pointermove', event => {
	if (!state.plane) return;
	const box = canvas.getBoundingClientRect();
	const x = state.x + (event.clientX - box.left - canvas.clientWidth / 2) * ratio() * state.mpp;
	const y = state.y + (event.clientY - box.top - canvas.clientHeight / 2) * ratio() * state.mpp;
	$('position').textContent = `${x.toFixed(1)}, ${y.toFixed(1)} µm`;
	if (!state.drag || state.drag.id !== event.pointerId) return;
	state.x -= (event.clientX - state.drag.x) * ratio() * state.mpp;
	state.y -= (event.clientY - state.drag.y) * ratio() * state.mpp;
	state.drag.x = event.clientX; state.drag.y = event.clientY; changed();
});
for (const name of ['pointerup', 'pointercancel', 'lostpointercapture']) canvas.addEventListener(name, () => {
	state.drag = null; canvas.classList.remove('dragging');
});

function navigateMini(event, clamp) {
	const box = miniBox(); if (!box) return false;
	const rect = mini.getBoundingClientRect();
	const x = (event.clientX - rect.left) * mini.width / rect.width;
	const y = (event.clientY - rect.top) * mini.height / rect.height;
	if (!clamp && (x < box.x || x > box.x + box.width || y < box.y || y > box.y + box.height)) return false;
	state.x = state.plane.x + Math.max(0, Math.min(box.width, x - box.x)) / box.scale;
	state.y = state.plane.y + Math.max(0, Math.min(box.height, y - box.y)) / box.scale;
	changed(); return true;
}
mini.addEventListener('pointerdown', event => {
	if (event.button !== 0 || state.opening || !navigateMini(event, false)) return;
	mini.setPointerCapture(event.pointerId); state.miniDrag = true; mini.classList.add('dragging');
});
mini.addEventListener('pointermove', event => { if (state.miniDrag) navigateMini(event, true); });
for (const name of ['pointerup', 'pointercancel', 'lostpointercapture']) mini.addEventListener(name, event => {
	if (state.miniDrag && name === 'pointerup') navigateMini(event, true);
	state.miniDrag = false; mini.classList.remove('dragging');
});

document.addEventListener('keydown', event => {
	if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'o') { event.preventDefault(); showOpen(); return; }
	if ($('open-dialog').open || $('generate-dialog').open || $('analyse-dialog').open || !state.plane || state.opening || /INPUT|SELECT|TEXTAREA/.test(event.target.tagName)) return;
	const key = event.key.toLowerCase();
	if (key === 'f') fit();
	else if (key === '1') nativeSize();
	else if (key === '+' || key === '=') zoom(1 / 1.25);
	else if (key === '-') zoom(1.25);
	else if (key.startsWith('arrow')) {
		if (key === 'arrowleft') state.x -= canvas.width * state.mpp / 4;
		if (key === 'arrowright') state.x += canvas.width * state.mpp / 4;
		if (key === 'arrowup') state.y -= canvas.height * state.mpp / 4;
		if (key === 'arrowdown') state.y += canvas.height * state.mpp / 4;
		changed();
	} else return;
	event.preventDefault();
});

new ResizeObserver(resize).observe($('stage'));
new ResizeObserver(resize).observe(mini);
window.addEventListener('resize', resize);
setupSidePanel();

async function start() {
	try {
		state.config = await (await api('/api/state')).json();
		$('root-label').textContent = state.config.root;
		showRootEditor(false);
		makeClient();
		showCache('client', null);
		globalThis.setInterval?.(() => { refreshCache(); }, kCacheRefreshMs);
		$('threads').value = state.config.threads;
		$('backend').options[1].disabled = !state.config.openslide;
		setupHalo();
		setupGenerate();
		setupAnalyse();
		$('connection').textContent = connectionLabel();
		if (!writable()) {
			for (const id of ['generate', 'make-gallery']) Object.assign($(id), {disabled: true, title: remoteWriteRefused});
		}
		status('Ready · Open an image to begin');
		resize();
// An image named on the server's command line wins over this browser's last.
		const remembered = state.config.initial_image ? null : rememberedImage();
		if (state.config.initial_image) { $('path').value = state.config.initial_image; requestOpen(); }
		else if (remembered) restoreImage(remembered);
		const running = state.config.analyse && await (await api('/api/analyse')).json();
		if (running?.running) { setAnalysing(true); pollAnalyse().catch(failAnalyse); }
	} catch (error) { status(error.message); $('connection').textContent = 'Connection unavailable'; }
}
start();
