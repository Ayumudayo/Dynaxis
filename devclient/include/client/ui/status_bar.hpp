#pragma once

#include <ftxui/dom/elements.hpp>

#include "client/app/app_state.hpp"

namespace client::ui {

ftxui::Element RenderStatusBar(const client::app::AppState& state);

} // namespace client::ui

