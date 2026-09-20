#pragma once
#include "common.hpp"

namespace nvs30::dxgi {
bool install_hooks();
void shutdown();
void set_internal_creation(bool value);
bool internal_creation();
}

