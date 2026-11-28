// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// Pvue's page side, a classic script defining globalThis.Pvue:
// - the frame rules every viewer uses: a view plus its halo band (haloRequest),
//   and when a prepared frame still serves a view (servesView, needsRender);
// - PvueClient, which drives the browser client (pvue_worker.js) for one slide.
// Views are requests {epoch, generation, plane, display, x, y, width, height,
// mpp} in microns; `display` asks for the display view around `plane` (a
// channel composite where the slide has one).

(function (global) {
'use strict';

// The view plus its halo band of `halo` percent of the view's raster width on
// every side, capped per axis at `maxEdge` pixels.
function haloRequest(view, halo, maxEdge) {
	const width = Math.ceil(view.width / view.mpp);
	const height = Math.ceil(view.height / view.mpp);
	const band = Math.ceil(width * halo / 100);
	const bandX = Math.max(0, Math.min(band, Math.floor((maxEdge - width) / 2))) * view.mpp;
	const bandY = Math.max(0, Math.min(band, Math.floor((maxEdge - height) / 2))) * view.mpp;
	return {...view, x: view.x - bandX, y: view.y - bandY, width: view.width + 2 * bandX,
		height: view.height + 2 * bandY, halo};
}

// A prepared rectangle of the current halo serves the view until the view has
// used up half of its actual halo on any side; without a halo, until the view
// leaves it.
function servesView(prepared, view, halo) {
	if (!prepared || prepared.epoch !== view.epoch || prepared.generation !== view.generation ||
		prepared.plane !== view.plane || Boolean(prepared.display) !== Boolean(view.display) ||
		prepared.mpp !== view.mpp || prepared.halo !== halo) return false;
	const marginX = Math.max(0, (prepared.width - view.width) / 4);
	const marginY = Math.max(0, (prepared.height - view.height) / 4);
	return prepared.x <= view.x - marginX && prepared.y <= view.y - marginY &&
		prepared.x + prepared.width >= view.x + view.width + marginX &&
		prepared.y + prepared.height >= view.y + view.height + marginY;
}

// The shown complete frame or the request in flight already serves the view.
function needsRender(frame, inflight, view, halo) {
	return !(frame?.complete && servesView(frame.request, view, halo)) && !servesView(inflight, view, halo);
}

// A URL template's {name} placeholders replaced by `values`, URL-encoded.
function fillTemplate(template, values) {
	return template.replace(/\{(\w+)\}/g, (match, key) => key in values ? encodeURIComponent(values[key]) : match);
}

// What a failure is remembered by: a plane, or the composite around it.
function viewKey(request) {
	return `${request.display ? 'display' : 'plane'}:${request.plane}`;
}

// Drives pvue_worker.js for the open slide. `options`: workerUrl, clientUrl,
// urls {geometry, tile, render} (templates; render without query), headers,
// tbfMiB and pbfMiB (cache budgets; pbfMiB 0: none), topology (0 arena, 1 per
// level), threads, and callbacks onOpened(composites), onFrame(message),
// onError(message), onCache(usage), onStarted(), onStartFailed(reason). Frames of each lane arrive in request order; an older one
// than the newest shown is dropped.
class PvueClient {
	constructor(options) {
		this.options = options;
		this.worker = null;
		this.generation = 0;
		this.ready = false;
		this.started = false;
		this.startFailed = null;
		this.failed = new Set();
		this.composites = [];
		this.next = 0;
		this.shown = {view: 0, overview: 0};
	}

	static available() { return typeof Worker !== 'undefined'; }

	// Starts the worker and its WebAssembly module, before any slide: `started`
	// becomes true (onStarted) once the module and its decode threads run, or
	// `startFailed` holds the reason (onStartFailed). Only a started client renders.
	start() {
		if (!PvueClient.available()) return false;
		if (!this.worker) {
			this.worker = new Worker(`${this.options.workerUrl}?client=${encodeURIComponent(this.options.clientUrl)}`);
			this.worker.onmessage = event => this.message(event.data);
		}
		return true;
	}

	open(generation) {
		this.ready = false;
		this.failed.clear();
		this.composites = [];
		this.generation = generation;
		if (!this.start()) return false;
		const {urls, headers, tbfMiB, pbfMiB, topology = 0, threads} = this.options;
		this.worker.postMessage({type: 'open', generation, urls, headers, tbfMiB, pbfMiB, topology, threads});
		return true;
	}

	// The browser renders this request itself.
	usable(request) {
		return this.ready && this.generation === request.generation && !this.failed.has(viewKey(request));
	}

	// The browser can at least decode the server's frame of this request's plane.
	decodes(request) {
		return this.ready && this.generation === request.generation && !request.display;
	}

	// Empties the open slide's caches, `caches` = 1 compressed | 2 decoded
	// (PYRA_CACHE_*); onCache answers with the usage after, onClearFailed(reason)
	// if the module refused.
	clear(caches) {
		if (this.worker && this.ready) this.worker.postMessage({type: 'clear', caches});
	}

	// Asks for the client's cache usage; onCache({compressed, decoded}) answers,
	// each {bytes, cap} (absent before the client has a slide).
	cacheUsage() {
		if (this.worker) this.worker.postMessage({type: 'cache'});
	}

	// `type` 'render' renders in the browser; 'server' decodes the server's frame.
	send(request, type = 'render', lane = 'view') {
		request.id = ++this.next;
		request.lane = lane;
		request.started = performance.now();
		this.worker.postMessage({type, request});
	}

	message(message) {
		if (message.type === 'started') {
			this.started = true;
			this.options.onStarted?.();
			return;
		}
		if (message.type === 'start-failed') {
			this.startFailed = message.message;
			this.options.onStartFailed?.(message.message);
			return;
		}
		if (message.generation !== this.generation) return;
		if (message.type === 'opened') {
			this.ready = true;
			this.composites = message.composites || [];
			this.options.onOpened?.(this.composites);
		} else if (message.type === 'error') {
			if (message.plane >= 0) this.failed.add(viewKey(message));
			else this.ready = false;
			this.options.onError?.(message);
		} else if (message.type === 'cache') {
			this.options.onCache?.(message);
		} else if (message.type === 'clear-failed') {
			this.options.onClearFailed?.(message.message);
		} else if (message.type === 'frame') {
			const lane = message.request.lane || 'view';
			if (message.request.id < this.shown[lane]) return;
			this.shown[lane] = message.request.id;
			this.options.onFrame?.(message);
		}
	}
}

global.Pvue = {haloRequest, servesView, needsRender, fillTemplate, viewKey, PvueClient};
})(globalThis);
