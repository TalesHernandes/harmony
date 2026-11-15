#define HARMONY_PANEL_POLICY_ONLY
#include "../src/panel-renderer.h"
#include <iostream>
using app::PanelCadence;
static_assert(app::PanelInterval(144, PanelCadence::Screen).count() == 16666);
static_assert(app::PanelInterval(60, PanelCadence::Screen).count() == 16666);
static_assert(app::PanelInterval(50, PanelCadence::Screen).count() == 20000);
static_assert(app::PanelInterval(24, PanelCadence::Screen).count() == 41666);
static_assert(app::PanelInterval(0, PanelCadence::Screen).count() == 1000000);
static_assert(app::PanelInterval(-1, PanelCadence::Camera).count() == 1000000);
static_assert(app::PanelInterval(144, PanelCadence::Camera).count() == 33333);
static_assert(app::PanelInterval(24, PanelCadence::Camera).count() == 41666);
int main() { std::cout << "PASS: panel cadence respects monitor, screen 60 Hz and camera/preview/PiP 30 Hz caps\n"; }
