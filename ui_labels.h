#pragma once

// LABELS sub-screen (reached from the setup screen): edit every
// installation-flavored string — header title, tile names, temp sensor
// names, tank names — persisted via gxSetLabel. Build once after
// uiSetupBuild; open pushes onto whatever screen is active.
void uiLabelsBuild();
void uiLabelsOpen();
