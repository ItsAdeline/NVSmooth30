#pragma once
#include "common.hpp"

namespace nvs30::pe {
struct Section {
    std::byte* begin{};
    std::size_t size{};
    DWORD characteristics{};
};

bool valid_image(HMODULE module);
std::size_t image_size(HMODULE module);
std::vector<Section> sections(HMODULE module);
bool address_in_image(HMODULE module, const void* address);
bool address_in_writable_section(HMODULE module, const void* address, std::size_t bytes);
void** find_import_slot(HMODULE module, std::string_view dll, std::string_view symbol);
void** find_delay_import_slot(HMODULE module, std::string_view dll, std::string_view symbol);
void** find_iat_value(HMODULE module, const void* value);
std::vector<void**> find_data_pointer_slots(HMODULE module, const void* value);
bool write_memory(void* destination, const void* source, std::size_t size);
bool write_pointer(void** slot, void* value, void** previous = nullptr);
}

