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

// UI labels: every installation-flavored string (header title, tile names,
// temp sensor names, tank names) as an NVS-overridable slot. Slot layout is
// the fixed slots below, then GX_TEMP_UNITS-many temp names, then
// GX_TANK_UNITS-many tank names. Labels are UI-task-only (built, edited,
// and rendered there) — no locking. An empty override reverts to the
// config.h/secrets.h compile-time default; kGxLblTitle's default "" keeps
// meaning "auto: follow the GX system name".
enum : int {
  kGxLblTitle = 0,
  kGxLblBatt,      // battery tile + its chart series
  kGxLblCurrent,
  kGxLblPowerIn,
  kGxLblAcLoads,
  kGxLblFixedCount
};
int gxLabelCount();
const char* gxLabel(int i);                       // effective text
const char* gxLabelDefault(int i);                // compile-time default
void gxLabelCaption(int i, char* out, int cap);   // "TITLE", "TEMP 1", ...
void gxSetLabel(int i, const char* txt);          // persist; "" = revert
