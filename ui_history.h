#pragma once

// Full-screen 24 h history chart, opened by tapping a tile on the power
// screen. One chart object, re-filled per series; CLOSE returns to the
// power screen. Refreshes itself once a minute while visible.
void uiHistBuild();          // create once, after lv_init
void uiHistOpen(int series); // HistSeries index (tile order)
