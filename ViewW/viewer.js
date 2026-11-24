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
const state = {
	config: null, image: null, plane: null, zPlanes: [], generation: 0, epoch: 0, revision: 0,
	x: 0, y: 0, mpp: 1, frame: null, overview: null,
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

// Enabled for an open DICOM slide when the server can run the bridge; while a
// run is going it stays enabled so the dialog can be reopened to watch it.
function updateAnalyseButton() {
	const analyse = state.config?.analyse;
	const path = state.openOptions?.path;
	const ready = Boolean(analyse?.available) && Boolean(state.image) && isDicom(path) && !state.opening;
	$('analyse').disabled = !ready && !state.analysing;
	$('analyse').textContent = state.analysing ? 'Analysing…' : 'Analyse…';
	$('analyse').title = !analyse?.available ? (analyse?.reason || 'Python processing is not available')
		: !state.image ? 'Open a DICOM slide first'
		: !isDicom(path) ? 'Analyse works on DICOM slides (.dcm) only'
		: 'Run Python scripts over the open DICOM slide into a new channel';
}

// After a reload the same channel and focal plane, if the slide still has it.
function matchingPlane(planes, previous) {
	return previous ? planes.find(plane => plane.channel === previous.channel && plane.z === previous.z) ?? null : null;
}

async function api(path, options = {}) {
	const response = await fetch(path, {...options, headers: {...options.headers, 'X-Pyra-Session': token}});
	if (!response.ok) {
		let message = `Server returned ${response.status}`;
		try { message = (await response.json()).error || message; } catch (_) { /* Keep HTTP error. */ }
		throw new Error(message);
	}
	return response;
}

function ratio() { return window.devicePixelRatio || 1; }
function dimensions() { return {width: canvas.width, height: canvas.height}; }
function selectedPlane() { return state.image?.planes[Number($('plane').value)]; }

function updateLabels() {
	if (!state.plane) return;
	$('zoom-label').textContent = `${(state.plane.mpp / state.mpp * 100).toFixed(1)}%`;
	$('resolution').textContent = `${state.mpp.toPrecision(4)} µm/px`;
}

function viewRequest() {
	const {width, height} = dimensions();
	const scale = Math.max(1, width / state.config.max_edge, height / state.config.max_edge);
	return {
		epoch: state.epoch, revision: state.revision, generation: state.generation,
		plane: state.plane.index, x: state.x - width * state.mpp / 2,
		y: state.y - height * state.mpp / 2, width: width * state.mpp,
		height: height * state.mpp, mpp: state.mpp * scale
	};
}

function changed() {
	++state.revision;
	updateLabels();
	draw();
	clearTimeout(state.timer);
	state.timer = setTimeout(() => {
		if (!state.plane || state.opening) return;
		if (clientUsable()) { sendClientView(viewRequest()); return; }
		if (clientDecodes()) { sendClientView(viewRequest(), 'server'); return; }
		state.pendingRender = viewRequest();
		pump();
	}, 35);
}

// Browser rendering (client_worker.js): the worker holds a WebAssembly copy of
// PyrA's reader, fetches the stored tiles a view needs and renders it here. The
// server's /api/render remains the path when the build has no client, before it
// is ready, and for a plane whose codec the client lacks.
const client = {worker: null, generation: 0, ready: false, next: 0, shown: 0, failedPlanes: new Set()};

function clientUsable() {
	return client.ready && client.generation === state.generation && Boolean(state.plane) &&
		!client.failedPlanes.has(state.plane.index);
}

// The client cannot render this plane but can decode the server's frame of it,
// which then travels as lossless HTJ2K instead of raw RGBA.
function clientDecodes() {
	return client.ready && client.generation === state.generation && Boolean(state.plane);
}

function startClient() {
	client.ready = false;
	client.failedPlanes.clear();
	if (!state.config?.client || typeof Worker === 'undefined') return;
	if (!client.worker) {
		client.worker = new Worker('client_worker.js');
		client.worker.onmessage = event => clientMessage(event.data);
	}
	client.generation = state.generation;
	client.worker.postMessage({type: 'open', token, generation: state.generation, cacheMiB: state.config.client_cache_mib,
		threads: navigator.hardwareConcurrency || 1});
}

// `type` 'render' renders in the browser; 'server' fetches the server's frame.
function sendClientView(request, type = 'render') {
	request.id = ++client.next;
	request.started = performance.now();
	client.worker.postMessage({type, request});
}

async function clientMessage(message) {
	if (message.generation !== client.generation) return;
	if (message.type === 'opened') {
		client.ready = true;
		changed();
	} else if (message.type === 'error') {
		if (message.plane >= 0) client.failedPlanes.add(message.plane);
		else client.ready = false;
		status(`Browser rendering unavailable (${message.message}); the server renders instead.`);
		changed();
	} else if (message.type === 'frame') {
		const request = message.request;
// Any frame of this plane newer than the one shown helps: draw() places a frame
// by its own rectangle, so one from a slightly older view still lands right.
		if (request.epoch !== state.epoch || request.id < client.shown) return;
		const bitmap = await createImageBitmap(new ImageData(new Uint8ClampedArray(message.pixels), message.width,
			message.height), {colorSpaceConversion: 'none'});
		if (request.epoch !== state.epoch || request.id < client.shown) { bitmap.close(); return; }
		client.shown = request.id;
		state.frame?.bitmap.close();
		state.frame = {bitmap, request};
		draw();
		$('render-time').textContent = `${message.ms.toFixed(2)} ms (${message.server ? 'server' : 'browser'})`;
		$('total-time').textContent = `${(performance.now() - request.started).toFixed(2)} ms`;
		$('transfer').textContent = message.server ? `${(message.bytes / 1048576).toFixed(2)} MiB (HTJ2K frame)`
			: `${(message.bytes / 1048576).toFixed(2)} MiB in ${message.tiles} tiles`;
		$('raster').textContent = `${message.width} × ${message.height}`;
		status(message.complete ? state.notice || 'Ready · Drag to pan, scroll to zoom' : 'Loading tiles…');
	}
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
	state.zPlanes = state.image.planes.filter(plane => plane.channel === state.plane.channel)
		.sort((a, b) => a.z - b.z);
	const slider = $('z-slider');
	slider.max = state.zPlanes.length - 1;
	slider.value = state.zPlanes.findIndex(plane => plane.index === state.plane.index);
	const label = state.plane.z_is_index ? `Z plane ${state.plane.z + 1}` : `Z ${state.plane.z} µm`;
	$('z-position').textContent = label;
	slider.setAttribute('aria-valuetext', label);
	$('z-control').hidden = state.zPlanes.length < 2;
}

function selectPlane(preserveView = false) {
	state.plane = selectedPlane();
	updateZSlider();
	++state.epoch;
	state.pendingRender = null;
	state.frame?.bitmap.close();
	state.overview?.close();
	state.frame = state.overview = null;
	state.pendingOverview = {generation: state.generation, plane: state.plane.index, overview: 1, epoch: state.epoch};
	if (preserveView) changed(); else fit();
	pump();
}

async function pixels(request) {
	const params = new URLSearchParams(Object.entries(request).filter(([key]) => key !== 'epoch' && key !== 'revision'));
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
	state.image = image;
	state.generation = image.generation;
	startClient();
	$('plane').replaceChildren(...image.planes.map(plane => {
		const option = document.createElement('option');
		option.value = plane.index;
		option.textContent = `${plane.channel || 'Default'} · ${plane.z_is_index ? `Z plane ${plane.z + 1}` : `Z ${plane.z} µm`}`;
		return option;
	}));
	const defaultIndex = Number(image.properties.find(([key]) => key === 'pyra.default-plane-index')?.[1] ?? 0);
	const kept = task.reload ? matchingPlane(image.planes, state.plane) : null;
	if (kept) $('plane').value = kept.index;
	else if (image.planes.some(plane => plane.index === defaultIndex)) $('plane').value = defaultIndex;
	state.openOptions = {...task.options};
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
			if (task.epoch !== state.epoch || (kind === 'view' && task.revision !== state.revision)) frame.bitmap.close();
			else if (kind === 'overview') {
				state.overview?.close(); state.overview = frame.bitmap; drawMini();
			} else {
				state.frame?.bitmap.close(); state.frame = {bitmap: frame.bitmap, request: task}; draw();
				$('render-time').textContent = `${frame.nativeMs.toFixed(2)} ms`;
				$('total-time').textContent = `${(performance.now() - frame.started).toFixed(2)} ms`;
				$('transfer').textContent = `${(frame.bytes / 1048576).toFixed(2)} MiB`;
				$('raster').textContent = `${frame.width} × ${frame.height}`;
				status(frame.incomplete ? 'Incomplete render: unavailable image areas are black.' : state.notice || 'Ready · Drag to pan, scroll to zoom');
			}
		}
	} catch (error) {
		if (task.epoch === state.epoch) {
			status(error.message);
			if (kind === 'open') {
				$('open-error').textContent = error.message;
				state.opening = false;
				updateControls();
				if (state.plane) changed();
			}
		}
	} finally {
		state.busy = false;
		$('open-submit').disabled = state.opening;
		$('connection').textContent = 'Local connection';
		pump();
	}
}

function requestOpen(backend = $('backend').value) {
	if (state.opening) return;
	state.notice = null;
	state.opening = true;
	updateControls();
	state.pendingOverview = state.pendingRender = null;
	clearTimeout(state.timer);
	++state.epoch;
	state.pendingOpen = {epoch: state.epoch, options: {
		path: $('path').value, backend, tbf_mib: $('tbf').value,
		pbf_mib: $('pbf').value, threads: $('threads').value
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
	clearTimeout(state.timer);
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
	} catch (error) { $('open-error').textContent = error.message; }
}

function showOpen() {
	if (!state.config) return;
	if (!$('open-dialog').open) $('open-dialog').showModal();
	$('open-error').textContent = '';
	showFolder(state.folder);
}

for (const id of ['browse', 'welcome-open']) $(id).addEventListener('click', showOpen);
for (const id of ['close-dialog', 'cancel-dialog']) $(id).addEventListener('click', () => $('open-dialog').close());
$('open-form').addEventListener('submit', event => { event.preventDefault(); requestOpen(); });
$('parent-folder').addEventListener('click', () => showFolder(state.folder.split('/').slice(0, -1).join('/')));
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
		['generate-tile', synthetic.tile, synthetic.tile_min, synthetic.tile_max, synthetic.tile_step],
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
	updateGenerateFormat(synthetic.codec);
}

function updateGenerateFormat(preferredCodec) {
	const dicom = $('generate-format').value === 'dicom';
	const codecs = state.config.synthetic.codecs.filter(codec => !dicom || codec.dicom);
	$('generate-codec').replaceChildren(...codecs.map(codec => {
		const option = document.createElement('option');
		option.value = codec.id; option.textContent = codec.name;
		return option;
	}));
	const preferred = codecs.find(codec => String(codec.id) === String(preferredCodec));
	$('generate-codec').value = preferred?.id ?? codecs[0]?.id ?? '';
	const name = $('generate-name');
	if (name.value)
		name.value = name.value.replace(/\.(?:ptf|dcm)$/i, '') + (dicom ? '.dcm' : '.ptf');
	updateGenerateQuality();
}

function updateGenerateQuality() {
	const codec = state.config.synthetic.codecs.find(item => String(item.id) === String($('generate-codec').value));
	const canLossless = Boolean(codec?.lossless);
	if (!canLossless) $('generate-lossless').checked = false;
	$('generate-lossless').disabled = state.generating || !canLossless;
	$('generate-quality').disabled = state.generating || $('generate-lossless').checked;
}

function setGenerating(generating) {
	state.generating = generating;
	for (const id of generateFields) $(id).disabled = generating;
	$('generate-submit').disabled = generating;
	$('generate').disabled = generating && !$('generate-dialog').open;
	updateGenerateQuality();
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
	status(`Generating ${progress.path} · ${progress.done} / ${progress.total || '…'} tiles`);
	if (progress.running) { setTimeout(() => pollGenerate().catch(failGenerate), 250); return; }
	setGenerating(false);
	if (progress.error) { failGenerate(new Error(progress.error)); return; }
	$('generate-dialog').close();
	status(`Generated ${progress.path}`);
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
$('generate-codec').addEventListener('change', updateGenerateQuality);
$('generate-lossless').addEventListener('change', updateGenerateQuality);
$('generate-close').addEventListener('click', () => $('generate-dialog').close());
$('generate-dialog').addEventListener('close', () => { $('generate').disabled = state.generating; });
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
$('plane').addEventListener('change', () => selectPlane(state.plane?.channel === selectedPlane()?.channel));
$('z-slider').addEventListener('input', () => {
	const plane = state.zPlanes[Number($('z-slider').value)];
	if (!plane || state.opening) return;
	$('plane').value = plane.index;
	selectPlane(true);
});
$('backend').addEventListener('change', () => { $('tbf').disabled = $('threads').disabled = $('backend').value === '1'; });
$('fit').addEventListener('click', fit);
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

async function start() {
	try {
		state.config = await (await api('/api/state')).json();
		$('root-label').textContent = state.config.root;
		$('tbf').value = state.config.tbf_mib;
		$('pbf').value = state.config.pbf_mib;
		$('threads').value = state.config.threads;
		$('backend').options[1].disabled = !state.config.openslide;
		setupGenerate();
		setupAnalyse();
		$('connection').textContent = 'Local connection';
		status('Ready · Open an image to begin');
		resize();
		if (state.config.initial_image) { $('path').value = state.config.initial_image; requestOpen(); }
		const running = state.config.analyse && await (await api('/api/analyse')).json();
		if (running?.running) { setAnalysing(true); pollAnalyse().catch(failAnalyse); }
	} catch (error) { status(error.message); $('connection').textContent = 'Connection unavailable'; }
}
start();
