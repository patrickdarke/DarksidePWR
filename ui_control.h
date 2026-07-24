#pragma once
#include "gx_modbus.h"

// CONTROL page: write access to the Victron system over the same Modbus
// path the display reads from. MultiPlus mode + shore input current limit,
// GX relays, DVCC max charge current, alternator on/off (newer GX firmware
// only). Every widget shows GX read-back state, not the last command — a
// write that didn't take is visible within a poll. MULTI OFF (kills AC
// loads) requires a second confirming tap.
void uiCtlBuild();                  // create the screen once, after lv_init
void uiCtlOpen();                   // switch to it
void uiCtlUpdate(const GxData& d);  // push fresh read-back state
void uiCtlDemoArmOff();  // arm the MULTI OFF confirm, exactly as a first tap
                         // would (serial 'D', for docs; no-op if armed)
