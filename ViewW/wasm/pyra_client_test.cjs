// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

// End-to-end check of the browser client against a running pyra_web_test:
// the WebAssembly client renders each view from the server's geometry and
// stored tiles, and its RGBA must equal the server's own /api/render frame.
//
// Usage: node pyra_client_test.cjs CLIENT_JS URL SLIDE [SLIDE ...]
// (SLIDE relative to the server's image root.) Prints bytes moved per view.

'use strict';

const kView = {width: 960, height: 540};
const kMissingBatch = 64;
const kMaxPasses = 12;
const kCacheMiB = 256;
// Largest per-sample difference attributed to float decoder rounding.
const kLossyRounding = 1;
// A browser's connections per host: tile fetches in flight at once.
const kConcurrentFetches = 6;

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
	const createPyraClient = require(clientJs);
	const module = await createPyraClient();
	const html = await (await fetch(base)).text();
	const token = html.match(/name="pyra-session" content="([^"]+)"/)[1];
	const headers = {'X-Pyra-Session': token};
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
		const opened = module._pyra_client_open(pointer, geometry.length, kCacheMiB, require('node:os').availableParallelism());
		module._free(pointer);
		if (opened !== 0) throw new Error(`${slide}: open ${module.UTF8ToString(module._pyra_client_error())}`);
		const defaultPlane = Number(image.properties.find(([key]) => key === 'pyra.default-plane-index')?.[1] ?? 0);
		const plane = image.planes[defaultPlane];
		const fit = Math.max(plane.width / kView.width, plane.height / kView.height);
		let tiles = 0, bytes = 0, views = 0, mismatches = 0, rounded = 0, passes = 0, htj2kBytes = 0, rawBytes = 0;
		for (const mpp of [fit, fit / 4, Math.max(plane.mpp, fit / 16), plane.mpp]) {
			const view = {x: plane.x + plane.width * 0.4 - kView.width * mpp / 2, y: plane.y + plane.height * 0.45 - kView.height * mpp / 2,
				width: kView.width * mpp, height: kView.height * mpp, mpp};
			let status = -1;
			for (let pass = 0; pass < kMaxPasses; ++pass) {
				++passes;
				status = module._pyra_client_render(plane.index, view.x, view.y, view.width, view.height, view.mpp);
				if (status !== 0 && status !== 6) throw new Error(`${slide}: render ${module.UTF8ToString(module._pyra_client_error())}`);
				let moved = 0;
				for (;;) {
					const count = module._pyra_client_take_missing(kMissingBatch);
					if (count <= 0) break;
					const ids = Array.from({length: count}, (_, i) => [
						module.HEAPU32[(module._pyra_client_missing() >> 2) + 2 * i],
						module.HEAPU32[(module._pyra_client_missing() >> 2) + 2 * i + 1]]);
					const bodies = await fetchAll(ids, ([level, tile]) =>
						api(`/api/raw-tile?generation=${image.generation}&level=${level}&tile=${tile}`).then(r => r.arrayBuffer()));
					ids.forEach(([level, tile], i) => {
						const data = new Uint8Array(bodies[i]);
						const at = module._malloc(Math.max(1, data.length));
						module.HEAPU8.set(data, at);
						const put = module._pyra_client_put(level, tile, at, data.length);
						module._free(at);
						if (put !== 0) throw new Error(`${slide}: put ${module.UTF8ToString(module._pyra_client_error())}`);
						bytes += data.length;
					});
					tiles += count;
					moved += count;
				}
				if (moved === 0) break;
			}
			const width = module._pyra_client_width(), height = module._pyra_client_height();
			const local = module.HEAPU8.slice(module._pyra_client_pixels(), module._pyra_client_pixels() + width * height * 4);
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
			const decoded = module._pyra_client_decode(at, frame.length);
			module._free(at);
			if (decoded !== 0) throw new Error(`${slide}: decode ${module.UTF8ToString(module._pyra_client_error())}`);
			const unpacked = module.HEAPU8.slice(module._pyra_client_pixels(), module._pyra_client_pixels() + served.length);
			if (module._pyra_client_width() * module._pyra_client_height() * 4 !== served.length ||
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
		module._pyra_client_close();
		const verdict = mismatches === 0 ? 'PASS' : 'FAIL';
		if (mismatches) ++failed;
		console.log(`test_web_client_matches_server ${slide}: ${verdict} (${views} views, ${mismatches} differ, ` +
			`${rounded} within rounding, HTJ2K frames ${(rawBytes / htj2kBytes).toFixed(1)}x smaller and lossless, ` +
			`${tiles} tiles, ${(bytes / 1048576).toFixed(2)} MiB, ${passes} passes, ${(performance.now() - started).toFixed(0)} ms)`);
	}
	process.exit(failed ? 1 : 0);
}

main().catch(error => { console.error(error.message); process.exit(1); });
