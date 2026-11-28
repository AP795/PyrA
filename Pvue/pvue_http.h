// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

// What a server feeding Pvue's browser client agrees on with it, whatever its
// HTTP stack: the geometry and stored tiles are addressed by a slide generation
// (a stale one is answered 410 Gone), immutable, and served by enough workers.

namespace pyra::pvue {

// Both raster axes of a rendered view; the client renders within the same bound.
inline constexpr auto kMaxRenderEdge = 4096u;
// The longest edge of an overview (minimap) render.
inline constexpr auto kOverviewEdge = 256u;
//
// HTTP workers, and requests that may wait for one. cpp-httplib holds a worker
// for each open keep-alive connection. A browser keeps six per host, but other
// clients open new sockets while idle ones still hold workers (Node's fetch: 55 s
// for a pass that takes 1.7 s with enough workers), and a request queued behind
// idle connections waits for the keep-alive timeout. Workers are cheap here:
// slide access is serialized anyway.
inline constexpr auto kHttpWorkerThreads = 32u;
inline constexpr auto kHttpQueuedRequests = 64u;
// Idle keep-alive. Longer than clients wait before reusing an idle socket (Node's
// fetch: 4 s), so a request never lands on a socket the server is just closing;
// the workers above already outnumber a browser's connections.
inline constexpr auto kKeepAliveSeconds = 5;
// Stored tiles and geometry are addressed by the slide generation, so a URL's
// content never changes: browsers may keep it this long without revalidating.
inline constexpr auto kImmutableCacheSeconds = 31536000u;
// Each of the browser client's tile caches (compressed and decoded). A view must
// fit: tiles are put between renders, unpinned, and uncompressed CZI sub-blocks
// are large. A composite holds three channels' tiles.
inline constexpr auto kClientCacheMiB = 256u;
// The most a viewer may give each of the client's caches: both, with the frames
// being rendered, must fit the module's 2 GB memory (-sMAXIMUM_MEMORY,
// Pvue/wasm/CMakeLists.txt).
inline constexpr auto kClientCacheMaxMiB = 768u;
// The server's decoded-tile cache (PBF): none. A server sends stored tiles as
// they are and never decodes or composes for the viewer; its one-plane fallback
// render, for a browser whose client does not run, decodes afresh each time. Its
// compressed cache (TBF) keeps the reader's default: the tiles it sends are read
// through it, so a tile asked again is not read from the file again; a slide's
// missing coarse levels are built through it at open, and a gallery member must
// fit it.
inline constexpr auto kServerDecodedCacheMiB = 0u;
// The status answering a geometry or tile request for a slide no longer open.
inline constexpr auto kStaleGenerationStatus = 410;

} // namespace pyra::pvue
