#pragma once

namespace netease {

class Lyrics;

// All widgets are created once; a timer renders the shared model.
void ui_create(const Lyrics *lyrics);
void ui_destroy();
// Minimal screen when the app cannot start (workspace busy, no memory).
void ui_show_unavailable(const char *text);

}  // namespace netease
