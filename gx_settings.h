#pragma once
#include <WString.h>

// Runtime settings (setup screen; NVS namespace "darkside", config.h
// supplies first-boot defaults). All setters are safe to call from the UI
// task: state is guarded by this module's own mutex, and target changes are
// handed to the poller task via a pending slot rather than applied inline.

void gxSettingsLoad();  // once from setup(), before tasks/UI events

bool gxTempsInF();                   // current display unit
void gxSetTempsInF(bool fahrenheit); // apply now + persist

const char* gxGetTarget();           // current GX target (mDNS host or IP)
void gxSetTarget(const char* addr);  // "" reverts to the config.h default

// Poller-side: apply a posted target change (true once per change; logs
// "[setup] gx target set..."), and copy the current target for resolving.
bool gxTargetConsumePending();
void gxTargetCopy(String& host, bool& overridden);
