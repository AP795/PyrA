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
async function viewer(dicom = true) {
	const elements = new Map();
	function element(id) {
		if (!elements.has(id)) elements.set(id, {
			value: '0', width: 640, height: 480, clientWidth: 640, clientHeight: 480,
			listeners: {}, attributes: {}, options: [{}, {}], children: [],
			addEventListener(name, handler) { this.listeners[name] = handler; },
			close() { this.open = false; }, showModal() { this.open = true; },
			setAttribute(name, value) { this.attributes[name] = value; },
			getContext() { return new Proxy({}, {get: () => () => {}}); },
			replaceChildren(...children) { this.children = children; }
		});
		return elements.get(id);
	}
	const config = {max_edge: 4096, root: '.', tbf_mib: 32, pbf_mib: 32, threads: 4, synthetic: {
		codecs: [{id: 1, name: 'JPEG', lossless: false, dicom},
			{id: 4, name: 'JPEG XL', lossless: true, dicom: false}],
		width: 4096, height: 4096, tile: 256, z: 4, mpp: 0.5, codec: 1, quality: 90, tile_min: 64,
		tile_max: 2048, tile_step: 16, edge_max: 1048576, z_max: 16, mpp_min: 0.05, mpp_max: 10,
		z_step: 1, z_step_min: 0.1, z_step_max: 5, focus: 5 / 12, nucleus: 8, nucleus_min: 3, nucleus_max: 40,
		spacing: 12, spacing_min: 4, spacing_max: 200, positive: 0.3, tumour: 0.4, lymphocytes: 0.4}};
	const context = vm.createContext({
		document: {getElementById: element, querySelector: () => ({content: 'test'}),
			addEventListener() {}, createElement: () => ({})},
		window: {devicePixelRatio: 1, addEventListener() {}},
		ResizeObserver: class { observe() {} },
		fetch: async () => ({ok: true, json: async () => config}),
		setTimeout: () => 1, clearTimeout() {}, URLSearchParams
	});
	vm.runInContext(fs.readFileSync(path.join(__dirname, 'viewer.js'), 'utf8'), context);
	await new Promise(setImmediate);
	const runtime = vm.runInContext('({state, selectPlane, updateControls, viewRequest, setGenerating, doOpen, ' +
		'matchingPlane, analyseMessage, analyseTables, updateAnalyseButton})', context);
	const planes = [
		{index: 0, channel: 'G', z: 9, mpp: 1, x: 0, y: 0, width: 1000, height: 1000},
		{index: 1, channel: 'DAPI', z: 2, mpp: 1, x: 0, y: 0, width: 1000, height: 1000},
		{index: 2, channel: 'G', z: -3, mpp: 1, x: 0, y: 0, width: 1000, height: 1000},
		{index: 3, channel: 'G', z: 2, mpp: 1, x: 0, y: 0, width: 1000, height: 1000}
	];
	Object.assign(runtime.state, {image: {planes}, busy: true, generation: 7});
	element('plane').value = '2';
	runtime.selectPlane();
	return {...runtime, element, context};
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
