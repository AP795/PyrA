<!--
PyrA Project, Git ap795/PyrA
Copyright (c) 2026 Alexandre Papine
SPDX-License-Identifier: MIT
-->

# Pvue

Pvue is the display layer viewers share above PyrA's reader. A server only sends
a slide's geometry and its tiles as they are stored; Pvue, in the viewer, decides
which planes make the picture, renders them and turns them into RGBA8. The PyrA
Web Viewer (`ViewW/`) is its first user.

Pvue reads pixels through the public C API only (`pyra_get_image`), so the same
code renders a slide opened from its file and the browser client's remote
("sister") slide, which holds the stored tiles a server sent. Core's internal
headers supply the composition rule (`compose_rgb_from_channels`,
`narrow_to_8bit`), header-only; Core itself knows nothing of Pvue.

| Part | Files |
|---|---|
| Display rule | `view_compose.h/.cpp`, `display_rgba.h` |
| What a server agrees on | `pvue_http.h` |
| Cache use a viewer shows | `pvue_cache.h` |
| Browser client (WebAssembly) | `wasm/pvue_client.cpp` |
| Browser page side | `web/pvue_worker.js`, `web/pvue_view.js` |

## The display rule

`display_recipe(planes, gallery, reference)` names the planes of the view around
the plane the viewer has chosen (`reference`):

- a named grey channel (Gray8/Gray16) is composed with the channels named R, G
  and B (ASCII, any case, the first of each) at its own focal position, when all
  three exist: `Channels`, in R, G, B order;
- any other plane is displayed on its own: `Rgb8` as stored, `Rgb16` narrowed
  from its range, `Gray` scaled from its range, `None` for other formats (shown
  by `display_rgba`, Float32 included);
- a gallery (property `gallery.members`) is `Gallery`, single planes only: its
  fields are composed per acquired field, from tile coverage the rule does not
  see yet.

`render_display(wsi, recipe, view, frame)` renders each recipe plane and composes
a `Channels` recipe through `compose_rgb_from_channels`; it is INCOMPLETE when any
plane is, and the missing tiles of all three accumulate. `render_plane` renders
one plane through `display_rgba`. Both rules scale integer samples identically:
`display_rgba` truncates `v * 255.0 / range`, `narrow_to_8bit` computes
`v * 255 / range` in integers (`test_display_rgba_equals_narrow_to_8bit`).

## Serving the client

A server feeds the client with two routes of its own naming and the constants
of `pvue_http.h`:

- the geometry blob (`pyra_get_geometry`, or Core's `serialize_remote_geometry`);
- one stored tile (`pyra_read_encoded_tile`, or `read_remote_tile`), an empty
  body for a sparse tile.

Both are addressed by a slide generation and immutable
(`kImmutableCacheSeconds`); a stale generation is answered 410. Every response
carries `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`: the client's decode threads share
WebAssembly memory, which a browser allows only on a cross-origin isolated page
in a secure context. A one-plane server render (any route) is optional: the
client decodes a lossless HTJ2K frame of it for a plane whose codec it lacks. The
server never composes.

A server sends stored tiles through its compressed cache (TBF), which keeps the
reader's default: a tile read from the file stays in a slot, so a tile asked
again (another browser, or one whose own cache evicted it) is copied from there.
Missing coarse levels are built through it at open, and a gallery member must
fit it. A server does not decode for the viewer, so its decoded-tile cache (PBF)
is `kServerDecodedCacheMiB`, 0; the optional one-plane render, for a browser
whose client does not run, decodes afresh each time.

## The page side

Load `pvue_view.js` (a classic script defining `globalThis.Pvue`), then:

```js
const client = new Pvue.PvueClient({
	workerUrl: 'pvue_worker.js', clientUrl: 'pvue_client.js',
	urls: {geometry: '/api/geometry?generation={generation}',
		tile: '/api/raw-tile?generation={generation}&level={level}&tile={tile}',
		render: '/api/render'},
	headers: {'X-Pyra-Session': token}, tbfMiB: 256, pbfMiB: 256, topology: 0,
	threads: navigator.hardwareConcurrency,
	onOpened: composites => {}, onFrame: message => {}, onError: message => {}});
client.open(generation);
client.send({epoch, generation, plane, display: true, x, y, width, height, mpp});
```

- `tbfMiB` and `pbfMiB` are the client's compressed and decoded cache budgets
  (`pbfMiB` 0: none; each at most `kClientCacheMaxMiB`), `topology` its cache
  layout (0 arena, 1 per level); they apply at the next `open`.
  `clear(caches)` empties the open slide's caches (1 compressed, 2 decoded;
  `pvue_client_clear`, `pyra_clear_cache`) and answers through `onCache`, or
  `onClearFailed(reason)`.
- URL templates take `{generation}`, `{level}` and `{tile}`; `render` gets the
  query appended. The worker sends `headers` with every request; cookies go with
  them as usual for same-origin requests.
- `onOpened` receives the slide's distinct colour composites,
  `[{reference, planes: [r, g, b]}]`.
- A request with `display: true` renders the display view around `plane`. Each
  pass posts a frame (`complete` false while tiles arrive) of RGBA8 pixels,
  placed by its request's rectangle in microns.
- Requests travel in lanes (`send(request, 'render', 'overview')` for a minimap):
  a newer request supersedes an older one of its lane between passes, and passes
  of different lanes never overlap.
- `cacheUsage()` asks for the client's cache usage; `onCache` receives
  `{compressed, decoded}`, each `{bytes, cap}` (allocated tile storage and its
  budget), absent before a slide is open.
- `start()` starts the worker and its module before any slide (`open` does it
  too). `started` turns true (`onStarted`) once the module and its decode threads
  run; otherwise `startFailed` holds the reason (`onStartFailed`): not
  cross-origin isolated, no WebAssembly SIMD, or threads that did not start within
  20 s (the Claude app's browser pane starts none). Only a started client
  renders.
- `usable(request)` says whether the browser renders it, `decodes(request)`
  whether it can at least decode the server's frame of its plane; a failure is
  remembered per plane or per composite (`viewKey`).
- `Pvue.haloRequest(view, halo, maxEdge)`, `Pvue.servesView` and
  `Pvue.needsRender` are the frame rules: a view plus its halo band, reused until
  half the halo is used up.

## Building the client

The client is a separate Emscripten build of this tree (Windows paths shown;
any host with emsdk works):

```powershell
. C:\c\emsdk\emsdk_env.ps1
emcmake cmake -G Ninja -S Deps -B ../out/Pyra/build/wasm-deps -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../out/Pyra/build/wasm-deps/install
cmake --build ../out/Pyra/build/wasm-deps
cmake --install ../out/Pyra/build/wasm-deps
emcmake cmake -G Ninja -S . -B ../out/Pyra/build/wasm -DCMAKE_BUILD_TYPE=Release -DPYRA_LINUX_SDK=../out/Pyra/build/wasm-deps/install -DPYR_WITH_OPENJPH=ON -DPYR_WITH_JXL=ON -DPYR_WITH_DCMTK=OFF -DPYR_WITH_CZI=OFF -DPYRA_WITH_OPENSLIDE=OFF -DPYRA_BUILD_BRIDGE=OFF -DPYRA_BUILD_WEB_TEST=OFF -DPYRA_BUILD_GUI_TEST=OFF -DBUILD_TESTING=OFF -DPYRA_ENABLE_IPO=OFF -DPYRA_ENABLE_AVX2=OFF
cmake --build ../out/Pyra/build/wasm --target pvue_client
```

It was developed with emsdk 4.0.20. The codec SDK includes the bundled libjxl
0.11.0 (`Deps/libjxl`), so JPEG XL needs no separate step. Every object of the
client is built with threads (`-pthread`, libjxl's
`JPEGXL_ENABLE_WASM_THREADS`), as WebAssembly shared memory requires; libjxl
itself runs single-threaded per tile, the client's pool decoding tiles in
parallel. For a client without JPEG XL, pass `-DPYRA_DEPENDENCY_JXL=OFF` to the
SDK and `-DPYR_WITH_JXL=OFF` to the client; its JPEG XL planes are then
rendered by the server.

The SDK and the client are built with WebAssembly SIMD (`-msimd128`), which
Highway (libjxl), OpenJPH and the compiler's autovectorizer use. Measured
against the same client without it (median client time of the four-view pass,
3-5 runs): lossy JPEG XL 191 to 145 ms; lossless JPEG XL 187 to 180 ms, JPEG
2000 508 to 494 ms, and JPEG XR, JPEG, HTJ2K and NDPI within run-to-run noise;
the client grew from 3.06 to 3.18 MB. A browser without wasm SIMD
(Chrome < 91, Firefox < 89, Safari < 16.4) cannot compile the module and gets
server rendering, like a page that is not cross-origin isolated.

The build writes `pvue_client.js` and `pvue_client.wasm` to `<wasm build>/Pvue/wasm`,
with the libjxl notices in `third-party/`. Each `.js` loads only its own `.wasm`,
so copy them together. Rebuild them after changing what the client contains:
- the geometry and tile formats (`Core/remote_pyramid.*`, the CZI sub-block
  descriptor, synthetic-tile encodings);
- the exports `pvue_worker.js` calls (`Pvue/wasm/CMakeLists.txt`);
- decoding, rendering or display code (`Pvue/view_compose.*`);
- the tile caches (`Core/wsi_cache.h`, `Core/wsi_buffers.cpp`).

No native build rebuilds the client. A native viewer configured with
`PYRA_WEB_CLIENT_DIR` pointing into a Ninja build of this tree warns on every
build when any source, header, library or CMake file of the client is newer than
`pvue_client.wasm`, and lists them (`ViewW/check_client.cmake`, about 0.4 s). A
client copied from elsewhere is not checked.

A geometry version mismatch makes the client refuse the slide, and the server
renders instead.
`node Pvue/wasm/pvue_client_test.cjs <wasm build>/Pvue/wasm/pvue_client.js
http://127.0.0.1:PORT SLIDE...` compares the client's renders with a running
PyrA_Web_Viewer's, and on a slide with R, G and B channels checks that the
colour composite carries each channel's own render.

