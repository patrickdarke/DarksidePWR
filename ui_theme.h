#pragma once
#include <stdint.h>

// DSODash design-language palette, shared by every screen (ui.cpp power
// screen, ui_setup.cpp Wi-Fi setup). Header constants so each screen keeps
// the short kName spelling.
constexpr uint32_t kBg = 0x000000;
constexpr uint32_t kTile = 0x111826;
constexpr uint32_t kRing = 0x1D2634;
constexpr uint32_t kText = 0xE8F0FA;
constexpr uint32_t kMuted = 0x8C96AA;
constexpr uint32_t kTeal = 0x46C8A5;
constexpr uint32_t kGreen = 0x30C860;
constexpr uint32_t kAmber = 0xFFC85A;
constexpr uint32_t kBlue = 0x4696FF;
constexpr uint32_t kRed = 0xFF5050;
