// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

// Exercise the shipped script and its event wiring with a minimal DOM. These
// checks do not replace browser layout, painting, or native range-input checks.
async function viewer(dicom = true, {globals = {}, client = false, local = true, responses = {}, initial = ''} = {}) {
	const elements = new Map();
	function element(id) {
		if (!elements.has(id)) elements.set(id, {
			value: '0', width: 640, height: 480, clientWidth: 640, clientHeight: 480,
			listeners: {}, attributes: {}, options: [{}, {}], children: [], classes: new Set(),
			style: {props: {}, setProperty(name, value) { this.props[name] = value; }},
			getBoundingClientRect() { return {left: 0, right: 1000, width: 1000}; },
			classList: {toggle(name, on) { const set = elements.get(id).classes; if (on) set.add(name); else set.delete(name); }},
			addEventListener(name, handler) { this.listeners[name] = handler; },
			close() { this.open = false; }, showModal() { this.open = true; },
			setAttribute(name, value) { this.attributes[name] = value; },
			getContext() { return new Proxy({}, {get: () => () => {}}); },
			replaceChildren(...children) { this.children = children; }
		});
		return elements.get(id);
	}
	const config = {max_edge: 4096, root: '/images', local, initial_image: initial, tbf_mib: 32, pbf_mib: 32, client, server_pbf_mib: 0, client_tbf_mib: 256, client_pbf_mib: 256, client_cache_max_mib: 768, threads: 4, view_halo: 20, view_halo_max: 50, synthetic: {
		codecs: [{id: 1, name: 'JPEG', lossless: false, lossy: true, dicom, tiled: true, untiled: true},
			{id: 4, name: 'JPEG XL', lossless: true, lossy: true, dicom: false, tiled: true, untiled: false},
			{id: 6, name: 'LZW', lossless: true, lossy: false, dicom: false, tiled: false, untiled: true}],
		width: 4096, height: 4096, tile: 256, z: 4, mpp: 0.5, codec: 1, quality: 90, tile_min: 64,
		tile_max: 2048, tile_step: 16, edge_max: 1048576, untiled_edge_max: 16384, z_max: 16, mpp_min: 0.05, mpp_max: 10,
		z_step: 1, z_step_min: 0.1, z_step_max: 5, focus: 5 / 12, nucleus: 8, nucleus_min: 3, nucleus_max: 40,
		spacing: 12, spacing_min: 4, spacing_max: 200, positive: 0.3, tumour: 0.4, lymphocytes: 0.4}};
// Armed timers are kept so a test can fire them; the script holds the id.
	const timers = [];
	const fetches = [];
// Form bodies sent, as [path, fields]; a function in `responses` answers its
// path with a whole response (a failure, say).
	const posted = [];
	const context = vm.createContext({
		document: {getElementById: element, querySelector: () => ({content: 'test'}),
			addEventListener() {}, createElement: () => ({})},
		window: {devicePixelRatio: 1, addEventListener() {}},
		ResizeObserver: class { observe() {} },
		fetch: async (url, options = {}) => {
			const path = String(url).split('?')[0];
			fetches.push(String(url));
			if (options.body instanceof URLSearchParams) posted.push([path, Object.fromEntries(options.body)]);
			if (typeof responses[path] === 'function') return responses[path]();
			return {ok: true, json: async () => path in responses ? responses[path] : config};
		},
		setTimeout: callback => timers.push(callback), clearTimeout() {}, URLSearchParams,
		performance: {now: () => 0}, ImageData: class { constructor(data, width, height) { Object.assign(this, {data, width, height}); } },
		createImageBitmap: async image => ({width: image.width, height: image.height, close() {}}),
		...globals
	});
	vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'Pvue', 'web', 'pvue_view.js'), 'utf8'), context);
	vm.runInContext(fs.readFileSync(path.join(__dirname, 'viewer.js'), 'utf8'), context);
	await new Promise(setImmediate);
	const runtime = vm.runInContext('({state, selectPlane, updateControls, viewRequest, setGenerating, doOpen, refreshCache, ' +
		'matchingPlane, analyseMessage, analyseTables, updateAnalyseButton, changed, haloRequest, needsRender, pump, ' +
		'clientOpened})', context);
	const planes = [
		{index: 0, channel: 'G', z: 9, mpp: 1, x: 0, y: 0, width: 1000, height: 1000},
		{index: 1, channel: 'DAPI', z: 2, mpp: 1, x: 0, y: 0, width: 1000, height: 1000},
		{index: 2, channel: 'G', z: -3, mpp: 1, x: 0, y: 0, width: 1000, height: 1000},
		{index: 3, channel: 'G', z: 2, mpp: 1, x: 0, y: 0, width: 1000, height: 1000}
	];
	for (const plane of planes) plane.key = String(plane.index);
	Object.assign(runtime.state, {image: {planes}, views: [...planes], busy: true, generation: 7});
	element('plane').value = '2';
	runtime.selectPlane();
	return {...runtime, element, context, timers, fetches, posted};
}

test('test_viewer_z_slider_uses_sorted_positions_in_current_channel', async () => {
	const {state, element} = await viewer();
	assert.deepEqual(Array.from(state.zPlanes, plane => plane.z), [-3, 2, 9]);
	assert.equal(element('z-slider').max, 2);
	assert.equal(element('z-slider').value, 0);
	assert.equal(element('z-control').hidden, false);
});

test('test_viewer_z_slider_updates_render_and_preserves_view', async () => {
	const {state, element, viewRequest} = await viewer();
	Object.assign(state, {x: 300, y: 400, mpp: 0.5, pendingRender: {plane: 2}});
	const epoch = state.epoch;
	element('z-slider').value = '1';
	element('z-slider').listeners.input();
	assert.equal(Number(element('plane').value), 3);
	assert.equal(state.plane.z, 2);
	assert.equal(state.plane.channel, 'G');
	assert.deepEqual([state.x, state.y, state.mpp], [300, 400, 0.5]);
	assert.equal(state.pendingRender, null);
	assert.equal(state.pendingOverview.plane, 3);
	assert.equal(state.pendingOverview.epoch, epoch + 1);
	assert.equal(viewRequest().plane, 3);
	assert.equal(element('z-position').textContent, 'Z 2 µm');
	assert.equal(element('z-slider').attributes['aria-valuetext'], 'Z 2 µm');
});

test('test_viewer_z_selector_sync_and_single_position_visibility', async () => {
	const {element} = await viewer();
	element('plane').value = '0';
	element('plane').listeners.change();
	assert.equal(element('z-slider').value, 2);
	element('plane').value = '1';
	element('plane').listeners.change();
	assert.equal(element('z-control').hidden, true);
	element('plane').value = '3';
	element('plane').listeners.change();
	assert.equal(element('z-control').hidden, false);
	assert.equal(element('z-slider').value, 1);
});

test('test_viewer_z_slider_disabled_while_opening', async () => {
	const {state, element, updateControls} = await viewer();
	state.opening = true;
	updateControls();
	assert.equal(element('z-slider').disabled, true);
	element('z-slider').value = '2';
	element('z-slider').listeners.input();
	assert.equal(state.plane.z, -3);
});

test('test_viewer_z_index_is_not_labelled_as_micrometres', async () => {
	const {state, element, selectPlane} = await viewer();
	for (const plane of state.image.planes) plane.z_is_index = true;
	element('plane').value = '0';
	selectPlane();
	assert.equal(element('z-position').textContent, 'Z plane 10');
	assert.equal(element('z-slider').attributes['aria-valuetext'], 'Z plane 10');
});

test('test_viewer_generate_lossless_follows_codec', async () => {
	const {element, setGenerating} = await viewer();
	assert.equal(element('generate-tile').step, 16);
	assert.equal(element('generate-z').max, 16);
	assert.equal(element('generate-lossless').disabled, true);
	element('generate-codec').value = '4';
	element('generate-codec').listeners.change();
	assert.equal(element('generate-lossless').disabled, false);
	element('generate-lossless').checked = true;
	element('generate-lossless').listeners.change();
	assert.equal(element('generate-quality').disabled, true);
	element('generate-codec').value = '1';
	element('generate-codec').listeners.change();
	assert.equal(element('generate-lossless').checked, false);
	assert.equal(element('generate-quality').disabled, false);
	setGenerating(true);
	assert.equal(element('generate-width').disabled, true);
	assert.equal(element('generate-submit').disabled, true);
});

test('test_viewer_generate_dicom_filters_codecs_and_changes_extension', async () => {
	const {element, setGenerating} = await viewer();
	assert.equal(element('generate-format').value, 'ptf');
	assert.equal(element('generate-dicom').disabled, false);
	element('generate-codec').value = '4';
	element('generate-codec').listeners.change();
	element('generate-lossless').checked = true;
	element('generate-name').value = 'cells.PTF';
	element('generate-format').value = 'dicom';
	element('generate-format').listeners.change();
	assert.equal(element('generate-name').value, 'cells.dcm');
	assert.deepEqual(element('generate-codec').children.map(option => option.value), [1]);
	assert.equal(element('generate-codec').value, 1);
	assert.equal(element('generate-lossless').disabled, true);
	assert.equal(element('generate-lossless').checked, false);
	setGenerating(true);
	assert.equal(element('generate-format').disabled, true);
	setGenerating(false);
	element('generate-format').value = 'ptf';
	element('generate-format').listeners.change();
	assert.equal(element('generate-name').value, 'cells.ptf');
	assert.deepEqual(element('generate-codec').children.map(option => option.value), [1, 4]);
});

test('test_viewer_generate_untiled_tile_zero_writes_tif', async () => {
	const {element} = await viewer();
	assert.equal(element('generate-tile').min, 0);
	element('generate-name').value = 'cells.ptf';
	element('generate-tile').value = '48';
	element('generate-tile').listeners.change();
	assert.equal(Number(element('generate-tile').value), 0);
	assert.equal(element('generate-name').value, 'cells.tif');
	assert.deepEqual(element('generate-codec').children.map(option => option.value), [1, 6]);
	assert.equal(Number(element('generate-z').value), 1);
	assert.equal(element('generate-z').disabled, true);
	assert.equal(element('generate-dicom').disabled, true);
	assert.equal(element('generate-width').max, 16384);
	element('generate-codec').value = '6';
	element('generate-codec').listeners.change();
	assert.equal(element('generate-lossless').checked, true);
	assert.equal(element('generate-lossless').disabled, true);
	assert.equal(element('generate-quality').disabled, true);
	element('generate-tile').value = '16';
	element('generate-tile').listeners.change();
	assert.equal(Number(element('generate-tile').value), 64);
	assert.equal(element('generate-name').value, 'cells.ptf');
	assert.deepEqual(element('generate-codec').children.map(option => option.value), [1, 4]);
	assert.equal(element('generate-z').disabled, false);
	assert.equal(element('generate-dicom').disabled, false);
});

test('test_viewer_generate_dicom_disabled_without_dicom_writer', async () => {
	const {element} = await viewer(false);
	assert.equal(element('generate-dicom').disabled, true);
	assert.match(element('generate-dicom').title, /DCMTK/);
});

test('test_viewer_generate_dicom_reopens_with_native_reader', async () => {
	const {state, element, context} = await viewer();
	element('backend').value = '1';
	element('generate-format').value = 'dicom';
	element('generate-format').listeners.change();
	let sentFormat;
	context.fetch = async (url, options) => {
		if (options.method === 'POST') {
			sentFormat = new URL(url, 'http://localhost').searchParams.get('format');
			return {ok: true};
		}
		return {ok: true, json: async () => ({running: false, path: 'cells.dcm', done: 5, total: 5})};
	};
	await element('generate-form').listeners.submit({preventDefault() {}});
	assert.equal(sentFormat, 'dicom');
	assert.equal(state.pendingOpen.options.path, 'cells.dcm');
	assert.equal(state.pendingOpen.options.backend, '0');
	assert.equal(element('backend').value, '1');
});

test('test_viewer_analyse_enabled_only_for_open_dicom', async () => {
	const {state, element, updateAnalyseButton} = await viewer();
	state.config.analyse = {available: true, reason: '', cli: true, cores: 8, workers: 4};
	state.openOptions = {path: 'slides/a.ptf'};
	updateAnalyseButton();
	assert.equal(element('analyse').disabled, true);
	assert.match(element('analyse').title, /DICOM/);
	state.openOptions = {path: 'slides/a.DCM'};
	updateAnalyseButton();
	assert.equal(element('analyse').disabled, false);
	state.config.analyse = {available: false, reason: 'No scripts folder'};
	updateAnalyseButton();
	assert.equal(element('analyse').disabled, true);
	assert.equal(element('analyse').title, 'No scripts folder');
});

const analyseStats = {channels: ['DAB', 'DAB.roi'], chunks: {total: 132, processed: 87, skipped: 45, mask_only: 0},
	workers: 10, busy_workers: 6.7, frames: 9971, encoded_bytes: 105e6, files: 16,
	time: {opening: 0.8, prepare: 1.8, chunks: 14.75, assemble: 3.77, publish: 0, total: 21.12},
	work: {read: 4.88, python: 99.24, transfer: 5.76, encode: 8.93, masks: 4.35, spool: 0.28}};

test('test_viewer_analyse_message_names_mode_workers_and_channels', async () => {
	const {analyseMessage} = await viewer();
	assert.equal(analyseMessage({mode: 'cli', wall_s: 21.24, workers: 10, stats: analyseStats}),
		'Analysed by CLI in 21.2 s with 10 Python workers: DAB, DAB.roi');
});

test('test_viewer_analyse_tables_time_each_phase', async () => {
	const {analyseTables: tablesIn} = await viewer();
// The viewer runs in its own context: copy its arrays into this one to compare them.
	const analyseTables = progress => JSON.parse(JSON.stringify(tablesIn(progress)));
	const tables = analyseTables({mode: 'server', wall_s: 21.1, workers: 10, stats: analyseStats});
	assert.deepEqual(tables.map(([caption]) => caption),
		['Result', 'Wall time', 'Chunk work, summed over the 10 Python workers']);
	const [, wall] = tables[1];
	assert.deepEqual(wall[0], ['Opening and checks', '0.80 s', '4 %']);
	assert.deepEqual(wall[2], ['Chunks (Python workers)', '14.75 s', '70 %']);
	assert.deepEqual(wall[6], ['Total seen by the viewer', '21.10 s', '']);
	assert.equal(tables[0][1].some(([label]) => label.includes('kept masks')), false);
	const masks = analyseTables({mode: 'cli', wall_s: 1, workers: 2,
		stats: {...analyseStats, chunks: {...analyseStats.chunks, mask_only: 3}}});
	assert.deepEqual(masks[0][1][3], ['… of them still written for kept masks', '3']);
	assert.deepEqual(tables[2][1][6], ['Workers busy on average', '6.7 of 10']);
});

// A localStorage stand-in holding `items`.
function storage(items = {}) {
	return {items, getItem(key) { return key in this.items ? this.items[key] : null; },
		setItem(key, value) { this.items[key] = String(value); }, removeItem(key) { delete this.items[key]; }};
}

const rememberedOptions = {path: 'svs/a.svs', backend: '0', threads: '2'};
// The cache fields an open sends with the default settings and the native reader.
const defaultCacheFields = {tbf_mib: '32', pbf_mib: '0', topology: '0'};

test('test_viewer_side_panel_default_width', async () => {
	const {element} = await viewer(true, {globals: {localStorage: storage()}});
	assert.equal(element('main').style.props['--side-width'], '360px');
});

test('test_viewer_side_panel_restores_remembered_width', async () => {
	const {element} = await viewer(true, {globals: {localStorage: storage({'pyra.sideWidth': '420'})}});
	assert.equal(element('main').style.props['--side-width'], '420px');
});

test('test_viewer_side_panel_restore_not_capped_by_layout_at_load', async () => {
	const {element} = await viewer(true, {globals: {localStorage: storage({'pyra.sideWidth': '700'})}});
	assert.equal(element('main').style.props['--side-width'], '700px');
});

test('test_viewer_side_panel_drag_resizes_and_remembers', async () => {
	const localStorage = storage();
	const {element} = await viewer(true, {globals: {localStorage}});
	const splitter = element('splitter');
	const event = clientX => ({pointerId: 1, clientX, preventDefault() {}});
	splitter.listeners.pointerdown(event(640));
	splitter.listeners.pointermove(event(500));
	assert.equal(element('main').style.props['--side-width'], '500px');
	assert.equal(localStorage.items['pyra.sideWidth'], undefined);
	splitter.listeners.pointerup(event(550));
	assert.equal(element('main').style.props['--side-width'], '450px');
	assert.equal(localStorage.items['pyra.sideWidth'], '450');
});

test('test_viewer_side_panel_drag_keeps_grab_offset', async () => {
	const {element} = await viewer(true, {globals: {localStorage: storage()}});
	const splitter = element('splitter');
	const event = clientX => ({pointerId: 1, clientX, preventDefault() {}});
	splitter.listeners.pointerdown(event(644));
	splitter.listeners.pointermove(event(644));
	assert.equal(element('main').style.props['--side-width'], '360px');
	splitter.listeners.pointerup(event(544));
	assert.equal(element('main').style.props['--side-width'], '460px');
});

test('test_viewer_side_panel_width_clamped', async () => {
	const localStorage = storage();
	const {element} = await viewer(true, {globals: {localStorage}});
	const splitter = element('splitter');
	const event = clientX => ({pointerId: 1, clientX, preventDefault() {}});
	splitter.listeners.pointerdown(event(640));
	splitter.listeners.pointerup(event(50));
	assert.equal(element('main').style.props['--side-width'], '600px');
	splitter.listeners.pointerdown(event(400));
	splitter.listeners.pointerup(event(990));
	assert.equal(element('main').style.props['--side-width'], '240px');
});

test('test_viewer_side_panel_double_click_resets', async () => {
	const localStorage = storage({'pyra.sideWidth': '500'});
	const {element} = await viewer(true, {globals: {localStorage}});
	element('splitter').listeners.dblclick();
	assert.equal(element('main').style.props['--side-width'], '360px');
	assert.equal(localStorage.items['pyra.sideWidth'], undefined);
});

test('test_viewer_side_panel_arrow_keys_resize', async () => {
	const localStorage = storage();
	const {element} = await viewer(true, {globals: {localStorage}});
	element('splitter').listeners.keydown({key: 'ArrowLeft', preventDefault() {}});
	assert.equal(element('main').style.props['--side-width'], '376px');
	assert.equal(localStorage.items['pyra.sideWidth'], '376');
});

test('test_viewer_remembers_opened_image', async () => {
	const localStorage = storage();
	const {state, doOpen, context} = await viewer(true, {globals: {localStorage}});
	context.fetch = async () => ({ok: true, json: async () => ({generation: 9, open_ms: 1, planes: state.image.planes, properties: []})});
	await doOpen({epoch: state.epoch, options: {...rememberedOptions, ...defaultCacheFields}});
	assert.deepEqual(JSON.parse(localStorage.items['pyra.openedImage']), {root: '/images', options: rememberedOptions});
});

// The opens a test's viewer sent: the form of each /api/open.
const opens = posted => posted.filter(([route]) => route === '/api/open').map(([, form]) => form);
const remembered = root => storage({'pyra.openedImage': JSON.stringify({root, options: rememberedOptions})});
const openFails = () => ({ok: false, status: 400, json: async () => ({error: 'No such file'})});

test('test_viewer_restores_remembered_image_at_start', async () => {
	const {posted} = await viewer(true, {globals: {localStorage: remembered('/images')}, responses: {'/api/open': openFails}});
	assert.deepEqual(opens(posted), [{...rememberedOptions, ...defaultCacheFields}]);
});

test('test_viewer_ignores_image_remembered_under_another_root', async () => {
	const {posted} = await viewer(true, {globals: {localStorage: remembered('/elsewhere')}, responses: {'/api/open': openFails}});
	assert.deepEqual(opens(posted), []);
});

test('test_viewer_command_line_image_wins_over_remembered', async () => {
	const {posted} = await viewer(true, {globals: {localStorage: remembered('/images')}, initial: 'given.ptf',
		responses: {'/api/open': openFails}});
	assert.deepEqual(opens(posted).map(form => form.path), ['given.ptf']);
});

test('test_viewer_forgets_remembered_image_that_fails', async () => {
	const localStorage = remembered('/images');
	const {element, posted} = await viewer(true, {globals: {localStorage}, responses: {'/api/open': openFails}});
	assert.equal(opens(posted).length, 1);
	assert.equal('pyra.openedImage' in localStorage.items, false);
	assert.equal(element('open-error').textContent, 'No such file');
});

test('test_viewer_analyse_reload_preserves_view_and_plane', async () => {
	const {state, element, doOpen, matchingPlane, context} = await viewer();
	element('plane').value = '3';
	Object.assign(state, {x: 300, y: 400, mpp: 0.5});
	state.plane = state.image.planes[3];
	const planes = [
		{index: 0, channel: 'DAB', z: 0, mpp: 1, x: 0, y: 0, width: 1000, height: 1000},
		{index: 1, channel: 'G', z: 2, mpp: 1, x: 0, y: 0, width: 1000, height: 1000},
		{index: 2, channel: 'G', z: 9, mpp: 1, x: 0, y: 0, width: 1000, height: 1000}];
	assert.equal(matchingPlane(planes, {channel: 'G', z: 2}).index, 1);
	assert.equal(matchingPlane(planes, {channel: 'R', z: 2}), null);
	context.fetch = async () => ({ok: true, json: async () => ({generation: 9, open_ms: 1, planes, properties: []})});
	await doOpen({epoch: state.epoch, options: {path: 'slide.dcm'}, reload: true});
	assert.equal(Number(element('plane').value), 1);
	assert.equal(state.plane.channel, 'G');
	assert.deepEqual([state.x, state.y, state.mpp], [300, 400, 0.5]);
	assert.equal(state.openOptions.path, 'slide.dcm');
	assert.equal(state.generation, 9);
});

// Fires the render timer armed by the last navigation; null when none is armed.
function fireRender(timers) {
	const callback = timers.pop();
	timers.length = 0;
	callback?.();
	return callback ?? null;
}

test('test_viewer_halo_request_extends_view_by_fraction', async () => {
	const {state, viewRequest, haloRequest} = await viewer();
	const view = viewRequest();
// 640 x 480 raster: a 128-pixel band on every side.
	const band = 128 * view.mpp;
	const halo = haloRequest(view);
	assert.equal(halo.x, view.x - band);
	assert.equal(halo.y, view.y - band);
	assert.equal(halo.width, view.width + 2 * band);
	assert.equal(halo.height, view.height + 2 * band);
	assert.equal(halo.mpp, view.mpp);
	assert.equal(halo.plane, state.plane.index);
});

test('test_viewer_halo_capped_at_max_edge', async () => {
	const {haloRequest} = await viewer();
	const halo = haloRequest({plane: 2, x: 0, y: 0, width: 4000, height: 1000, mpp: 1});
	assert.deepEqual([halo.x, halo.y, halo.width, halo.height], [-48, -800, 4096, 2600]);
});

test('test_viewer_pan_within_half_halo_sends_no_render', async () => {
	const {state, timers, changed, viewRequest} = await viewer();
	assert.ok(fireRender(timers));
	const sent = state.pendingRender;
	assert.equal(sent, state.inflight);
	assert.ok(sent.width > viewRequest().width);
	state.x += 128 * state.mpp * 0.4;
	changed();
	assert.equal(timers.length, 0);
	assert.equal(state.pendingRender, sent);
});

test('test_viewer_pan_past_half_halo_requests_halo_render', async () => {
	const {state, timers, changed, viewRequest, haloRequest} = await viewer();
	fireRender(timers);
	const sent = state.pendingRender;
	state.x += 128 * state.mpp * 0.6;
	changed();
// Further motion neither postpones nor duplicates the armed render.
	state.x += 1;
	changed();
	assert.equal(timers.length, 1);
	fireRender(timers);
	assert.notEqual(state.pendingRender, sent);
	assert.deepEqual({...state.pendingRender}, {...haloRequest(viewRequest())});
});

test('test_viewer_stale_server_frame_is_still_shown', async () => {
	const {state, timers, context, changed, pump} = await viewer();
	fireRender(timers);
	const sent = state.pendingRender;
	state.x += 10 * state.mpp;
	changed();
	const width = Math.ceil(sent.width / sent.mpp);
	const height = Math.ceil(sent.height / sent.mpp);
	const headers = {'X-Pyra-Width': String(width), 'X-Pyra-Height': String(height), 'X-Pyra-Render-Ms': '1',
		'X-Pyra-Incomplete': '0'};
	context.fetch = async () => ({ok: true, headers: {get: name => headers[name] ?? null},
		arrayBuffer: async () => new ArrayBuffer(width * height * 4)});
	state.pendingOverview = null;
	state.busy = false;
	await pump();
	assert.equal(state.frame.request, sent);
	assert.equal(state.frame.complete, true);
});

test('test_viewer_halo_default_comes_from_server', async () => {
	const {state, element} = await viewer();
	assert.equal(state.halo, 20);
	assert.equal(element('halo').value, 20);
	assert.equal(element('halo').max, 50);
});

test('test_viewer_halo_field_sets_band_and_rerenders', async () => {
	const {state, timers, element, viewRequest} = await viewer();
	fireRender(timers);
	element('halo').value = '10';
	element('halo').listeners.change();
	assert.equal(state.halo, 10);
	assert.ok(fireRender(timers), 'A new halo did not re-render the served view');
	const view = viewRequest();
// 10% of a 640-pixel raster: a 64-pixel band.
	assert.equal(state.pendingRender.width, view.width + 2 * 64 * view.mpp);
	assert.equal(state.pendingRender.halo, 10);
});

test('test_viewer_halo_zero_renders_exact_view', async () => {
	const {state, timers, element, viewRequest} = await viewer();
	element('halo').value = '0';
	element('halo').listeners.change();
	fireRender(timers);
	const view = viewRequest();
	assert.deepEqual([state.pendingRender.x, state.pendingRender.y, state.pendingRender.width, state.pendingRender.height],
		[view.x, view.y, view.width, view.height]);
});

test('test_viewer_halo_field_clamped_to_server_limit', async () => {
	const {state, element} = await viewer();
	element('halo').value = '80';
	element('halo').listeners.change();
	assert.equal(state.halo, 50);
	assert.equal(element('halo').value, 50);
});

test('test_viewer_colour_composite_is_listed_first_and_selected', async () => {
	const {state, element, clientOpened, viewRequest} = await viewer();
	state.preferColour = true;
	clientOpened([{reference: 3, planes: [3, 2, 0]}]);
	assert.equal(element('plane').value, 'display:3');
	assert.equal(element('plane').children[0].textContent, 'Colour (R, G, B) · Z 2 µm');
	assert.equal(state.plane.display, true);
	assert.equal(viewRequest().display, true);
	assert.equal(viewRequest().plane, 3);
	assert.equal(element('z-control').hidden, true);
	element('plane').value = '1';
	element('plane').listeners.change();
	assert.equal(viewRequest().display, false);
});

test('test_viewer_colour_composite_not_forced_after_choice', async () => {
	const {state, element, clientOpened} = await viewer();
	state.preferColour = true;
	element('plane').value = '1';
	element('plane').listeners.change();
	clientOpened([{reference: 3, planes: [3, 2, 0]}]);
	assert.equal(element('plane').value, '1');
	assert.equal(state.plane.channel, 'DAPI');
});

// A browser with workers; the client reports its start through the worker.
const capable = {Worker: class { postMessage() {} }, WebAssembly: {}, crossOriginIsolated: true,
	navigator: {hardwareConcurrency: 4}};

// A value made in the viewer's realm, as this realm's plain data.
const plain = value => JSON.parse(JSON.stringify(value));

// Opens the Settings dialog; `fields` then sets its inputs by id suffix.
function settings(element, fields = {}) {
	element('settings').listeners.click();
	for (const [suffix, value] of Object.entries(fields)) {
		const field = element(`settings-${suffix}`);
		if (typeof value === 'boolean') field.checked = value;
		else field.value = String(value);
	}
}

// Applies the open Settings dialog and lets its requests settle.
async function apply(element) {
	element('settings-form').listeners.submit({preventDefault() {}});
	for (let i = 0; i < 3; ++i) await new Promise(setImmediate);
}

test('test_viewer_settings_defaults_for_native_reader', async () => {
	const {element} = await viewer(true, {globals: capable, client: true});
	settings(element);
	assert.equal(element('settings-dialog').open, true);
	assert.deepEqual(['server-tbf', 'server-pbf', 'server-type', 'browser-tbf', 'browser-pbf', 'browser-type']
		.map(id => element(`settings-${id}`).value), ['32', '0', '0', '256', '256', '0']);
	assert.equal(element('settings-browser-tbf').max, 768);
});

test('test_viewer_client_start_failure_reported', async () => {
	const {element, context} = await viewer(true, {globals: capable, client: true});
	vm.runInContext('client.message({type: "start-failed", message: "its decode threads did not start within 20 s"})', context);
	assert.match(element('status').textContent, /decode threads did not start/);
	assert.equal(element('cache-client-compressed').textContent, 'not running');
	settings(element);
	assert.equal(element('settings-browser-tbf').disabled, true);
	assert.equal(element('settings-browser-tbf').title, 'not running');
});

test('test_viewer_settings_browser_disabled_without_client_build', async () => {
	const {element} = await viewer(true, {globals: capable, client: false});
	assert.equal(element('cache-client-compressed').textContent, 'no browser client');
	settings(element);
	assert.equal(element('settings-browser-pbf').disabled, true);
	assert.equal(element('settings-browser-pbf').title, 'no browser client');
	assert.equal(element('settings-server-tbf').disabled, false);
});

test('test_viewer_settings_saved_and_used_by_the_client', async () => {
	const localStorage = storage();
	const first = await viewer(true, {globals: {...capable, localStorage}, client: true});
	settings(first.element, {'browser-tbf': 512, 'browser-type': 1, 'server-tbf': 64});
	await apply(first.element);
	assert.deepEqual(JSON.parse(localStorage.items['pyra.cacheSettings']),
		{server: {tbf: 64, pbf: null, type: null}, browser: {tbf: 512, pbf: null, type: 1}});
	const {element, context} = await viewer(true, {globals: {...capable, localStorage}, client: true});
	assert.deepEqual(plain(vm.runInContext('[client.options.tbfMiB, client.options.pbfMiB, client.options.topology]', context)),
		[512, 256, 1]);
	settings(element);
	assert.equal(element('settings-server-tbf').value, '64');
});

test('test_viewer_settings_server_pbf_follows_reader_unless_set', async () => {
	const localStorage = storage();
	const {element, context} = await viewer(true, {globals: {localStorage}});
	const pbf = backend => vm.runInContext(`serverCacheOptions('${backend}').pbf_mib`, context);
	assert.deepEqual([pbf('0'), pbf('1')], ['0', '32']);
	settings(element);
	await apply(element);
	assert.deepEqual([pbf('0'), pbf('1')], ['0', '32']);
	settings(element, {'server-pbf': 8});
	await apply(element);
	assert.deepEqual([pbf('0'), pbf('1')], ['8', '8']);
});

test('test_viewer_settings_size_change_reopens_the_image', async () => {
	const {element, state} = await viewer(true, {globals: {localStorage: storage()}});
	state.openOptions = {path: 'svs/a.svs', backend: '0', threads: '4', ...defaultCacheFields};
	settings(element, {'server-tbf': 64, 'server-type': 1, 'server-reset-pbf': true});
	await apply(element);
	assert.equal(state.pendingOpen.reload, true);
	assert.deepEqual(plain(state.pendingOpen.options), {path: 'svs/a.svs', backend: '0', threads: '4',
		tbf_mib: '64', pbf_mib: '0', topology: '1'});
	assert.equal(element('settings-dialog').open, false);
});

test('test_viewer_settings_reset_only_clears_in_place', async () => {
	const {element, state, posted} = await viewer(true, {globals: {localStorage: storage()},
		responses: {'/api/cache/clear': {freed: 1}}});
	state.openOptions = {path: 'svs/a.svs', backend: '0', threads: '4', ...defaultCacheFields};
	settings(element, {'server-reset-tbf': true, 'server-reset-pbf': true});
	await apply(element);
	assert.equal(state.pendingOpen, null);
	assert.deepEqual(posted.filter(([route]) => route === '/api/cache/clear'), [['/api/cache/clear', {compressed: '1', decoded: '1'}]]);
	assert.match(element('status').textContent, /Caches reset/);
});

test('test_viewer_settings_browser_reset_sent_to_client', async () => {
	const sent = [];
	const recording = {...capable, Worker: class { postMessage(message) { sent.push(message); } }};
	const {element, context} = await viewer(true, {globals: {...recording, localStorage: storage()}, client: true});
	vm.runInContext('client.message({type: "opened", generation: client.generation, composites: []})', context);
	settings(element, {'browser-reset-pbf': true});
	assert.equal(element('settings-browser-reset-pbf').disabled, false);
	await apply(element);
	assert.deepEqual(plain(sent.filter(message => message.type === 'clear')), [{type: 'clear', caches: 2}]);
});

test('test_viewer_settings_resets_need_an_open_image', async () => {
	const {element, state} = await viewer(true, {globals: {localStorage: storage()}});
	state.image = null;
	settings(element);
	assert.equal(element('settings-server-reset-tbf').disabled, true);
	assert.match(element('settings-note').textContent, /next one/);
});

test('test_viewer_client_cache_rows_say_starting', async () => {
	const {element} = await viewer(true, {globals: capable, client: true});
	assert.equal(element('cache-client-decoded').textContent, 'starting…');
});

test('test_viewer_root_change_hidden_for_network_connection', async () => {
	assert.equal((await viewer(true, {local: true})).element('change-root').hidden, false);
	assert.equal((await viewer(true, {local: false})).element('change-root').hidden, true);
});

test('test_viewer_root_change_updates_label_and_slide_path', async () => {
	const files = {path: '.', entries: [], truncated: false};
	const {state, element, fetches} = await viewer(true, {responses: {
		'/api/root': {root: 'D:/Data', slide: 'b/slide.dcm'}, '/api/files': files}});
	state.openOptions = {path: 'Data/b/slide.dcm'};
	element('change-root').listeners.click();
	assert.equal(element('root-editor').hidden, false);
	assert.equal(element('root-input').value, '/images');
	element('root-input').value = ' D:/Data ';
	element('set-root').listeners.click();
	await new Promise(setImmediate);
	assert.ok(fetches.includes('/api/root?path=D%3A%2FData'));
	assert.equal(element('root-label').textContent, 'D:/Data');
	assert.equal(state.openOptions.path, 'b/slide.dcm');
	assert.equal(state.folder, '');
	assert.equal(element('root-editor').hidden, true);
});

test('test_viewer_root_change_outside_slide_cannot_reload', async () => {
	const {state, element} = await viewer(true, {responses: {
		'/api/root': {root: 'E:/Other', slide: null}, '/api/files': {path: '.', entries: [], truncated: false}}});
	state.openOptions = {path: 'b/slide.dcm'};
	state.config.analyse = {available: true};
	element('set-root').listeners.click();
	await new Promise(setImmediate);
	assert.equal(state.openOptions, null);
	assert.equal(element('analyse').disabled, true);
	assert.equal(element('analyse').title, 'Reopen this slide from the current image directory');
});

test('test_viewer_root_change_reports_unsaved', async () => {
	const {element} = await viewer(true, {responses: {
		'/api/root': {root: 'D:/Data', slide: null, saved: false, save_error: 'Cannot write x'},
		'/api/files': {path: '.', entries: [], truncated: false}}});
	element('set-root').listeners.click();
	await new Promise(setImmediate);
	assert.equal(element('open-error').textContent, 'Changed, but not kept for the next start: Cannot write x');
});

test('test_viewer_folder_error_names_stopped_server', async () => {
	const {context, element} = await viewer();
	context.fetch = async () => { throw new TypeError('Failed to fetch'); };
	await vm.runInContext('showFolder("Philips")', context);
	assert.match(element('open-error').textContent, /Cannot reach the viewer server/);
});

test('test_viewer_cache_shows_server_usage_and_percent', async () => {
	const mib = 1024 * 1024;
	const {element, refreshCache} = await viewer(true, {responses: {
		'/api/cache': {compressed: {bytes: 8 * mib, cap: 32 * mib}, decoded: {bytes: 0, cap: 0},
			tiles: {read: 1234, served: 5600, served_bytes: 40 * mib}}}});
	await refreshCache();
	assert.equal(element('cache-server-compressed').textContent, '8.0 / 32 MiB · 25% full');
	assert.equal(element('tiles-read').textContent, '1,234');
	assert.equal(element('transfer').textContent, '5,600 tiles · 40.0 MiB');
});

test('test_viewer_cache_off_and_no_client_rows', async () => {
	const mib = 1024 * 1024;
	const {element, refreshCache} = await viewer(true, {responses: {
		'/api/cache': {compressed: {bytes: 0, cap: 32 * mib}, decoded: {bytes: 0, cap: 0},
			tiles: {read: 12, served: 0, served_bytes: 0}}}});
	await refreshCache();
	assert.equal(element('transfer').textContent, '0 tiles · 0.0 MiB');
	assert.equal(element('cache-server-decoded').textContent, 'off');
	assert.equal(element('cache-client-compressed').textContent, 'no browser client');
});

test('test_viewer_cache_marks_tiles_sent_past_the_cache', async () => {
	const mib = 1024 * 1024;
	const {element, refreshCache} = await viewer(true, {responses: {
		'/api/cache': {compressed: {bytes: 32 * mib, cap: 32 * mib, bypassed: 7}, decoded: {bytes: 0, cap: 0}}}});
	await refreshCache();
	const compressed = element('cache-server-compressed');
	assert.equal(compressed.textContent, '32 / 32 MiB · 100% full · 7 bypassed');
	assert.ok(compressed.classes.has('bypassed'));
	assert.match(compressed.title, /7 tiles were sent past this cache/);
});

test('test_viewer_cache_shows_hit_ratio', async () => {
	const mib = 1024 * 1024;
	const {element, refreshCache} = await viewer(true, {responses: {
		'/api/cache': {compressed: {bytes: 8 * mib, cap: 32 * mib, hits: 997, misses: 3},
			decoded: {bytes: 0, cap: 32 * mib, hits: 0, misses: 0}}}});
	await refreshCache();
	const compressed = element('cache-server-compressed');
	assert.equal(compressed.textContent, '8.0 / 32 MiB · 25% full · 99.7% hits');
	assert.equal(compressed.title, '997 hits, 3 misses since the image was opened.');
	assert.equal(element('cache-server-decoded').textContent, '0.0 / 32 MiB · 0% full');
});

test('test_viewer_cache_shows_tiles_held_and_average_size', async () => {
	const mib = 1024 * 1024;
	const {element, refreshCache} = await viewer(true, {responses: {
		'/api/cache': {compressed: {bytes: 32 * mib, cap: 32 * mib, held: 448, hits: 0, misses: 935},
			decoded: {bytes: 0, cap: 0, held: 0, hits: 0, misses: 0}}}});
	await refreshCache();
	const compressed = element('cache-server-compressed');
	assert.equal(compressed.textContent, '32 / 32 MiB · 448 tiles · 100% full · 0.0% hits');
	assert.equal(compressed.title, '448 tiles of 73.1 KiB on average, each stored at its own size. ' +
		'0 hits, 935 misses since the image was opened.');
	assert.equal(element('cache-server-decoded').textContent, 'off');
});

test('test_viewer_cache_hit_ratio_never_rounds_a_miss_to_100', async () => {
	const mib = 1024 * 1024;
	const {element, refreshCache} = await viewer(true, {responses: {
		'/api/cache': {compressed: {bytes: 8 * mib, cap: 32 * mib, hits: 99999, misses: 1}, decoded: {bytes: 0, cap: 0}}}});
	await refreshCache();
	assert.equal(element('cache-server-compressed').textContent, '8.0 / 32 MiB · 25% full · 99.9% hits');
});

test('test_viewer_cache_shows_browser_hit_ratio', async () => {
	const mib = 1024 * 1024;
	const {element, context} = await viewer(true, {globals: capable, client: true});
	vm.runInContext('client.message({type: "opened", generation: client.generation, composites: []})', context);
	context.message = {type: 'cache', compressed: {bytes: 4 * mib, cap: 256 * mib, hits: 1, misses: 3},
		decoded: {bytes: 64 * mib, cap: 256 * mib, hits: 30, misses: 10}};
	vm.runInContext('client.message({...message, generation: client.generation})', context);
	assert.equal(element('cache-client-compressed').textContent, '4.0 / 256 MiB · 2% full · 25.0% hits');
	assert.equal(element('cache-client-decoded').textContent, '64 / 256 MiB · 25% full · 75.0% hits');
});

test('test_viewer_cache_keeps_values_while_server_busy', async () => {
	const {element, refreshCache} = await viewer(true, {responses: {'/api/cache': {busy: true}}});
	element('cache-server-compressed').textContent = '1.0 / 32 MiB · 3% full';
	await refreshCache();
	assert.equal(element('cache-server-compressed').textContent, '1.0 / 32 MiB · 3% full');
});

test('test_viewer_overview_from_server_after_client_failure', async () => {
	const {state, context} = await viewer(true, {globals: capable, client: true});
	vm.runInContext('client.message({type: "opened", generation: client.generation, composites: []})', context);
	state.pendingOverview = null;
	vm.runInContext(`client.message({type: "error", generation: client.generation, plane: ${state.plane.index},
		display: false, lane: "overview", message: "vector"})`, context);
	assert.equal(state.pendingOverview?.overview, 1);
	assert.equal(state.pendingOverview?.plane, state.plane.index);
});
