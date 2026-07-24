#pragma once
#include "gx_modbus.h"

// Build the power screen (call once after lv_init + display registration)
// and push fresh values into it. uiSetLink drives the header status dot:
// 0 = no Wi-Fi, 1 = Wi-Fi up / GX not answering, 2 = live data.
void uiBuild();
void uiUpdate(const GxData& d);
void uiSetLink(int state);
