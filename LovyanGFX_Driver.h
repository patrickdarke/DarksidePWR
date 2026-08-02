#pragma once

// Board selector — not vendored itself. Each included file is ELECROW's
// lesson-03 config for that panel, unmodified (see CLAUDE.md board sections).
#if defined(BOARD_CROWPANEL_50)
#include "LovyanGFX_Driver_50.h"
#else
#include "LovyanGFX_Driver_35.h"
#endif
