// config.h - mm2gestures.ini loading.
#pragma once
#include <string>
#include "engine.h"

// Loads the INI at `path`, writing a commented default file first if it does
// not exist. Unknown or missing keys keep their defaults.
GestureConfig LoadConfig(const std::wstring &path);

// Human-readable one-screen summary of the active settings.
std::string DescribeConfig(const GestureConfig &c);
