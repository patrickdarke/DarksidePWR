#pragma once

// Board/install selector. config_35.h = truck, ELECROW CrowPanel Advance
// 3.5". config_50.h = the new second install on the ELECROW CrowPanel
// Advance 5.0" — different Victron device roster entirely, not just a
// bigger screen (see CLAUDE.md).
#if defined(BOARD_CROWPANEL_50)
#include "config_50.h"
#else
#include "config_35.h"
#endif
