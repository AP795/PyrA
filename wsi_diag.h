#pragma once

// Diagnostics accessors for the WSI loader, kept in their own header so a UI
// translation unit can ask for them without pulling in the whole loader.
//
// This existed because pyr.h named a parameter `slots`, which Qt defines as a
// keyword macro; that identifier is now slot_count, so including wsi.h from a
// Qt translation unit is no longer a hazard. The narrow header remains the
// better include.

namespace priml {

// Number of WsiLoader::Generate calls currently executing across all threads.
// Lets a display-side anomaly log tell whether a processing / scene-cache
// Generate overlapped a suspect backdrop render.
int wsi_generate_in_flight () noexcept;

} // namespace priml
