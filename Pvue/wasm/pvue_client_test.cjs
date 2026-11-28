// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// End-to-end check of Pvue's browser client against a running PyrA_Web_Viewer:
// the WebAssembly client renders each view from the server's geometry and
// stored tiles, and its RGBA must equal the server's own /api/render frame. On a
// slide with R, G and B channels, the client's colour composite must carry each
// channel's own render in its component (the server never composes).
//
// Usage: node pvue_client_test.cjs CLIENT_JS URL SLIDE [SLIDE ...]
// (SLIDE relative to the server's image root.) Prints bytes moved per view.

'use strict';

const kView = {width: 960, height: 540};
const kMissingBatch = 64;
const kMaxPasses = 12;
// Distinct budgets, so the report shows each cache's own.
const kTbfMiB = 256;
const kPbfMiB = 128;
const kArena = 0;
const kClearCompressed = 1;
// Largest per-sample difference attributed to float decoder rounding.
const kLossyRounding = 1;
// A browser's connections per host: tile fetches in flight at once.
const kConcurrentFetches = 6;
// DisplayKind::Channels (Pvue/view_compose.h).
const kDisplayChannels = 3;

async function fetchAll(ids, fetchOne) {
	const results = new Array(ids.length);
	let next = 0;
	const worker = async () => {
		while (next < ids.length) {
			const index = next++;
			results[index] = await fetchOne(ids[index]);
		}
	};
	await Promise.all(Array.from({length: Math.min(kConcurrentFetches, ids.length)}, worker));
	return results;
}

async function main() {
	const [clientJs, base, ...slides] = process.argv.slice(2);
	const createPvueClient = require(clientJs);
	const module = await createPvueClient();
	const page = await fetch(base);
	const html = await page.text();
	const token = html.match(/name="pyra-session" content="([^"]+)"/)[1];
// Node keeps no cookies: send the session cookie the page set, as a browser would.
	const session = (page.headers.get('set-cookie') || '').split(';')[0];
	const headers = {'X-Pyra-Session': token, Cookie: session};
	const api = async (path, options = {}) => {
// One retry, as a browser does when a reused keep-alive socket was just closed
// by the server (its keep-alive is shorter than Node's).
		const once = () => fetch(base + path, {...options, headers: {...headers, ...options.headers}});
		const response = await once().catch(once);
		if (!response.ok) throw new Error(`${path}: ${response.status} ${await response.text()}`);
		return response;
	};
	let failed = 0;
	for (const slide of slides) {
		const started = performance.now();
		const image = await (await api('/api/open', {method: 'POST', body: new URLSearchParams({path: slide, backend: 0})})).json();
		const geometry = new Uint8Array(await (await api(`/api/geometry?generation=${image.generation}`)).arrayBuffer());
		const pointer = module._malloc(geometry.length);
		module.HEAPU8.set(geometry, pointer);
		const opened = module._pvue_client_open(pointer, geometry.length, kTbfMiB, kPbfMiB, kArena,
			require('node:os').availableParallelism());
		module._free(pointer);
		if (opened !== 0) throw new Error(`${slide}: open ${module.UTF8ToString(module._pvue_client_error())}`);
		const defaultPlane = Number(image.properties.find(([key]) => key === 'pyra.default-plane-index')?.[1] ?? 0);
		const plane = image.planes[defaultPlane];
		const fit = Math.max(plane.width / kView.width, plane.height / kView.height);
		let tiles = 0, bytes = 0, views = 0, mismatches = 0, rounded = 0, passes = 0, htj2kBytes = 0, rawBytes = 0;
// Time in the client's own decode and render calls, without transfers.
		let clientMs = 0;
// Renders with `render` until no tile is missing; returns the RGBA frame.
		const complete = async render => {
			for (let pass = 0; pass < kMaxPasses; ++pass) {
				++passes;
				const renderStart = performance.now();
				const status = render();
				clientMs += performance.now() - renderStart;
				if (status !== 0 && status !== 6) throw new Error(`${slide}: render ${module.UTF8ToString(module._pvue_client_error())}`);
				let moved = 0;
				for (;;) {
					const count = module._pvue_client_take_missing(kMissingBatch);
					if (count <= 0) break;
					const ids = Array.from({length: count}, (_, i) => [
						module.HEAPU32[(module._pvue_client_missing() >> 2) + 2 * i],
						module.HEAPU32[(module._pvue_client_missing() >> 2) + 2 * i + 1]]);
					const bodies = await fetchAll(ids, ([level, tile]) =>
						api(`/api/raw-tile?generation=${image.generation}&level=${level}&tile=${tile}`).then(r => r.arrayBuffer()));
					ids.forEach(([level, tile], i) => {
						const data = new Uint8Array(bodies[i]);
						const at = module._malloc(Math.max(1, data.length));
						module.HEAPU8.set(data, at);
						const putStart = performance.now();
						const put = module._pvue_client_put(level, tile, at, data.length);
						clientMs += performance.now() - putStart;
						module._free(at);
						if (put !== 0) throw new Error(`${slide}: put ${module.UTF8ToString(module._pvue_client_error())}`);
						bytes += data.length;
					});
					tiles += count;
					moved += count;
				}
				if (moved === 0) break;
			}
			const width = module._pvue_client_width(), height = module._pvue_client_height();
			return module.HEAPU8.slice(module._pvue_client_pixels(), module._pvue_client_pixels() + width * height * 4);
		};
		const viewAt = mpp => ({x: plane.x + plane.width * 0.4 - kView.width * mpp / 2,
			y: plane.y + plane.height * 0.45 - kView.height * mpp / 2, width: kView.width * mpp, height: kView.height * mpp, mpp});
		for (const mpp of [fit, fit / 4, Math.max(plane.mpp, fit / 16), plane.mpp]) {
			const view = viewAt(mpp);
			const local = await complete(() =>
				module._pvue_client_render(plane.index, view.x, view.y, view.width, view.height, view.mpp));
			const params = new URLSearchParams({generation: image.generation, plane: plane.index, ...view});
			const response = await api(`/api/render?${params}`);
			const served = new Uint8Array(await response.arrayBuffer());
			++views;
			let largest = served.length === local.length ? 0 : 255, differing = 0;
			for (let i = 0; i < Math.min(served.length, local.length); ++i) {
				const d = Math.abs(served[i] - local[i]);
				if (d) { ++differing; largest = Math.max(largest, d); }
			}
			const compressed = await api(`/api/render?${params}&encoding=htj2k`);
			const frame = new Uint8Array(await compressed.arrayBuffer());
			if (compressed.headers.get('X-Pyra-Encoding') !== 'htj2k') throw new Error(`${slide}: frame was not HTJ2K`);
			const at = module._malloc(frame.length);
			module.HEAPU8.set(frame, at);
			const decoded = module._pvue_client_decode(at, frame.length);
			module._free(at);
			if (decoded !== 0) throw new Error(`${slide}: decode ${module.UTF8ToString(module._pvue_client_error())}`);
			const unpacked = module.HEAPU8.slice(module._pvue_client_pixels(), module._pvue_client_pixels() + served.length);
			if (module._pvue_client_width() * module._pvue_client_height() * 4 !== served.length ||
				unpacked.some((value, i) => value !== served[i]))
				throw new Error(`${slide}: the HTJ2K frame is not the raw frame`);
			htj2kBytes += frame.length;
			rawBytes += served.length;
			if (largest === 0) continue;
			const label = `  mpp ${mpp.toPrecision(4)}: ${differing} of ${served.length} samples differ, largest ${largest}`;
// A server render missing tiles is not a reference: its cache could not hold the view.
			if (response.headers.get('X-Pyra-Incomplete') === '1') {
				console.log(`${label} - server render incomplete, not compared`);
				continue;
			}
// Lossy JPEG 2000 / HTJ2K use float wavelets whose rounding differs between the
// x64 (SIMD) and WebAssembly builds of the codec: one code value at most.
			if (largest <= kLossyRounding) {
				console.log(`${label} - float decoder rounding`);
				++rounded;
				continue;
			}
			console.log(label);
			++mismatches;
		}
		if (module._pvue_client_recipe(plane.index) === kDisplayChannels) {
			const base = module._pvue_client_recipe_planes() >> 2;
			const rgb = Array.from({length: 3}, (_, i) => module.HEAPU32[base + i]);
			const view = viewAt(fit / 4);
			const composite = await complete(() =>
				module._pvue_client_render_display(plane.index, view.x, view.y, view.width, view.height, view.mpp));
			let differ = 0;
			for (const [component, index] of rgb.entries()) {
				const channel = await complete(() =>
					module._pvue_client_render(index, view.x, view.y, view.width, view.height, view.mpp));
				if (channel.length !== composite.length) throw new Error(`${slide}: a channel renders to another size`);
				for (let i = 0; i < channel.length; i += 4)
					if (composite[i + component] !== channel[i]) ++differ;
			}
			if (differ) ++failed;
			console.log(`test_web_client_composite_matches_channels ${slide}: ${differ ? 'FAIL' : 'PASS'} ` +
				`(planes ${rgb.join(', ')}, ${differ} components differ)`);
		}
// pvue_client_cache: bytes, cap, hits, misses of the compressed cache, then of the
// decoded, then the tiles each holds.
		const cacheUsage = () => {
			const usage = module._pvue_client_cache();
			return usage ? Array.from(module.HEAPF64.slice(usage >> 3, (usage >> 3) + 10)) : null;
		};
		const usage = cacheUsage();
		const [compressed, compressedCap, hits, misses, decoded, decodedCap, , , held] = usage ?? [];
		const cacheOk = usage && compressed > 0 && compressed <= compressedCap && decoded <= decodedCap &&
			compressedCap === kTbfMiB * 1048576 && decodedCap === kPbfMiB * 1048576 && misses > 0 && held > 0;
		if (!cacheOk) ++failed;
		console.log(`test_web_client_cache_usage ${slide}: ${cacheOk ? 'PASS' : 'FAIL'} (compressed ` +
			`${(compressed / 1048576).toFixed(2)} / ${compressedCap / 1048576} MiB, ${held} tiles, ${hits} hits ${misses} misses, ` +
			`decoded ${(decoded / 1048576).toFixed(2)} / ${decodedCap / 1048576} MiB)`);
		const clearOk = module._pvue_client_clear(kClearCompressed) === 0 && (() => {
			const [bytes, , clearedHits, clearedMisses, decodedAfter, , , , heldAfter] = cacheUsage();
			return bytes === 0 && clearedHits === 0 && clearedMisses === 0 && decodedAfter === decoded && heldAfter === 0;
		})();
		if (!clearOk) ++failed;
		console.log(`test_web_client_clear_compressed ${slide}: ${clearOk ? 'PASS' : 'FAIL'}`);
		module._pvue_client_close();
		if (module._pvue_client_cache() !== 0) { ++failed; console.log(`test_web_client_cache_usage ${slide}: FAIL (closed slide reports usage)`); }
		const verdict = mismatches === 0 ? 'PASS' : 'FAIL';
		if (mismatches) ++failed;
		console.log(`test_web_client_matches_server ${slide}: ${verdict} (${views} views, ${mismatches} differ, ` +
			`${rounded} within rounding, HTJ2K frames ${(rawBytes / htj2kBytes).toFixed(1)}x smaller and lossless, ` +
			`${tiles} tiles, ${(bytes / 1048576).toFixed(2)} MiB, ${passes} passes, client ${clientMs.toFixed(0)} ms, ` +
			`${(performance.now() - started).toFixed(0)} ms)`);
	}
	process.exit(failed ? 1 : 0);
}

main().catch(error => { console.error(error.message); process.exit(1); });
