// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

// Loads pvue_view.js as a page would, with a fake Worker recording its messages.
function pvue(globals = {}) {
	const workers = [];
	class FakeWorker {
		constructor(url) { this.url = url; this.posted = []; workers.push(this); }
		postMessage(message) { this.posted.push(message); }
	}
	const context = vm.createContext({Worker: FakeWorker, performance: {now: () => 0}, ...globals});
	vm.runInContext(fs.readFileSync(path.join(__dirname, 'pvue_view.js'), 'utf8'), context);
	return {Pvue: vm.runInContext('Pvue', context), workers};
}

const view = {epoch: 1, generation: 4, plane: 2, display: false, x: 100, y: 50, width: 640, height: 480, mpp: 1};

test('test_pvue_halo_request_extends_view_by_fraction', () => {
	const {Pvue} = pvue();
	const halo = Pvue.haloRequest(view, 20, 4096);
	assert.deepEqual([halo.x, halo.y, halo.width, halo.height, halo.halo], [-28, -78, 896, 736, 20]);
});

test('test_pvue_halo_request_capped_at_edge', () => {
	const {Pvue} = pvue();
	const halo = Pvue.haloRequest({...view, width: 4000, height: 1000}, 20, 4096);
	assert.equal(halo.width, 4096);
	assert.equal(halo.height, 1000 + 2 * 800);
});

test('test_pvue_serves_view_until_half_halo_used', () => {
	const {Pvue} = pvue();
	const prepared = Pvue.haloRequest(view, 20, 4096);
	assert.equal(Pvue.servesView(prepared, {...view, x: view.x + 60}, 20), true);
	assert.equal(Pvue.servesView(prepared, {...view, x: view.x + 70}, 20), false);
	assert.equal(Pvue.servesView(prepared, view, 10), false);
});

test('test_pvue_display_view_is_not_served_by_plane_frame', () => {
	const {Pvue} = pvue();
	const prepared = Pvue.haloRequest(view, 20, 4096);
	assert.equal(Pvue.servesView(prepared, {...view, display: true}, 20), false);
	assert.equal(Pvue.needsRender({complete: true, request: prepared}, null, {...view, display: true}, 20), true);
	assert.equal(Pvue.needsRender({complete: true, request: prepared}, null, view, 20), false);
});

test('test_pvue_fill_template_encodes_values', () => {
	const {Pvue} = pvue();
	assert.equal(Pvue.fillTemplate('/wsi/7/tile?v={generation}&level={level}&tile={tile}&x={unknown}',
		{generation: 'a b', level: 2, tile: 15}), '/wsi/7/tile?v=a%20b&level=2&tile=15&x={unknown}');
});

test('test_pvue_client_opens_worker_with_templates', () => {
	const {Pvue, workers} = pvue();
	const urls = {geometry: '/g?{generation}', tile: '/t?{level}&{tile}', render: '/r'};
	const client = new Pvue.PvueClient({workerUrl: 'w.js', clientUrl: 'c.js', urls, headers: {A: '1'}, tbfMiB: 8,
		pbfMiB: 0, topology: 1, threads: 2});
	assert.equal(client.open(4), true);
	assert.equal(workers[0].url, 'w.js?client=c.js');
	assert.deepEqual(JSON.parse(JSON.stringify(workers[0].posted[0])), {type: 'open', generation: 4,
		urls, headers: {A: '1'}, tbfMiB: 8, pbfMiB: 0, topology: 1, threads: 2});
});

test('test_pvue_client_clear_sent_once_open', () => {
	const {Pvue, workers} = pvue();
	const failures = [];
	const client = new Pvue.PvueClient({workerUrl: 'w.js', onClearFailed: reason => failures.push(reason)});
	client.open(4);
	client.clear(1);
	assert.equal(workers[0].posted.some(message => message.type === 'clear'), false);
	client.message({type: 'opened', generation: 4, composites: []});
	client.clear(3);
	assert.deepEqual(JSON.parse(JSON.stringify(workers[0].posted.at(-1))), {type: 'clear', caches: 3});
	client.message({type: 'clear-failed', generation: 4, message: 'busy'});
	assert.deepEqual(failures, ['busy']);
	assert.equal(client.ready, true);
});

test('test_pvue_client_failure_is_per_view_kind', () => {
	const {Pvue} = pvue();
	const errors = [];
	const client = new Pvue.PvueClient({workerUrl: 'w.js', onError: message => errors.push(message)});
	client.open(4);
	client.message({type: 'opened', generation: 4, composites: [{reference: 2, planes: [2, 1, 0]}]});
	assert.equal(client.usable({...view, display: true}), true);
	client.message({type: 'error', generation: 4, plane: 2, display: true, message: 'no'});
	assert.equal(client.usable({...view, display: true}), false);
	assert.equal(client.usable(view), true);
	assert.equal(client.decodes({...view, display: true}), false);
	assert.equal(errors.length, 1);
});

test('test_pvue_client_drops_older_frames_per_lane', () => {
	const {Pvue} = pvue();
	const frames = [];
	const client = new Pvue.PvueClient({workerUrl: 'w.js', onFrame: message => frames.push(message.request.id)});
	client.open(4);
	const frame = (id, lane) => ({type: 'frame', generation: 4, request: {id, lane}});
	client.message(frame(3, 'view'));
	client.message(frame(2, 'view'));
	client.message(frame(1, 'overview'));
	client.message({...frame(9, 'view'), generation: 3});
	assert.deepEqual(frames, [3, 1]);
});

test('test_pvue_client_start_reported_whatever_the_generation', () => {
	const {Pvue, workers} = pvue();
	const events = [];
	const client = new Pvue.PvueClient({workerUrl: 'w.js', onStarted: () => events.push('started'),
		onStartFailed: reason => events.push(reason)});
	assert.equal(client.start(), true);
	assert.equal(client.start(), true);
	assert.equal(workers.length, 1);
	client.message({type: 'started'});
	assert.equal(client.started, true);
	client.open(4);
	assert.equal(workers.length, 1);
	client.message({type: 'start-failed', message: 'no threads'});
	assert.equal(client.startFailed, 'no threads');
	assert.deepEqual(events, ['started', 'no threads']);
});

test('test_pvue_client_cache_usage_round_trip', () => {
	const {Pvue, workers} = pvue();
	const usages = [];
	const client = new Pvue.PvueClient({workerUrl: 'w.js', onCache: usage => usages.push(usage)});
	client.open(4);
	client.cacheUsage();
	assert.equal(workers[0].posted[1].type, 'cache');
	client.message({type: 'cache', generation: 4, compressed: {bytes: 1, cap: 2}, decoded: {bytes: 0, cap: 2}});
	client.message({type: 'cache', generation: 3, compressed: {bytes: 9, cap: 9}});
	assert.equal(usages.length, 1);
	assert.equal(usages[0].compressed.cap, 2);
});
