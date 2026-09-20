#pragma once
#include "common.hpp"

namespace nvs30 {
void log_open();
void log_close();
void logf(const char* format, ...);
}

