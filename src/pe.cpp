#include "nvs30/pe.hpp"

#include <cctype>

namespace nvs30::pe {
namespace {
IMAGE_NT_HEADERS64* nt_headers(HMODULE module) {
    if (!module) return nullptr;
    auto* base = reinterpret_cast<std::byte*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;
    return nt;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}
}

bool valid_image(HMODULE module) { return nt_headers(module) != nullptr; }

std::size_t image_size(HMODULE module) {
    const auto* nt = nt_headers(module);
    return nt ? nt->OptionalHeader.SizeOfImage : 0;
}

std::vector<Section> sections(HMODULE module) {
    std::vector<Section> out;
    auto* nt = nt_headers(module);
    if (!nt) return out;
    auto* base = reinterpret_cast<std::byte*>(module);
    auto* first = IMAGE_FIRST_SECTION(nt);
    const std::size_t image = nt->OptionalHeader.SizeOfImage;
    out.reserve(nt->FileHeader.NumberOfSections);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = first[i];
        const std::size_t requested = std::max<std::size_t>(s.Misc.VirtualSize, s.SizeOfRawData);
        const std::size_t bounded = s.VirtualAddress < image
            ? std::min<std::size_t>(requested, image - s.VirtualAddress) : 0;
        out.push_back({base + s.VirtualAddress,
                       bounded,
                       s.Characteristics});
    }
    return out;
}

bool address_in_image(HMODULE module, const void* address) {
    const auto* base = reinterpret_cast<const std::byte*>(module);
    const auto* p = reinterpret_cast<const std::byte*>(address);
    const auto size = image_size(module);
    return size && p >= base && p < base + size;
}

bool address_in_writable_section(HMODULE module, const void* address, std::size_t bytes) {
    const auto* p = reinterpret_cast<const std::byte*>(address);
    for (const auto& s : sections(module)) {
        if (!(s.characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        if (p >= s.begin && bytes <= s.size && p <= s.begin + s.size - bytes) return true;
    }
    return false;
}

void** find_import_slot(HMODULE module, std::string_view dll, std::string_view symbol) {
    auto* nt = nt_headers(module);
    if (!nt) return nullptr;
    auto* base = reinterpret_cast<std::byte*>(module);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return nullptr;
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* dll_name = reinterpret_cast<const char*>(base + desc->Name);
        if (!iequals(dll_name, dll)) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base +
            (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (symbol == reinterpret_cast<const char*>(import->Name))
                return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

void** find_delay_import_slot(HMODULE module, std::string_view dll, std::string_view symbol) {
    auto* nt = nt_headers(module);
    if (!nt) return nullptr;
    auto* base = reinterpret_cast<std::byte*>(module);
    const auto image = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress || dir.Size < 32 ||
        static_cast<std::size_t>(dir.VirtualAddress) + dir.Size > image)
        return nullptr;

    // IMAGE_DELAYLOAD_DESCRIPTOR / ImgDelayDescr consists of eight DWORDs.
    // PE32+ delay imports emitted by the MSVC linker use RVA-based fields
    // (Attributes bit 0). Keeping the local layout avoids depending on
    // delayimp.h in the public headers.
    struct DelayDescriptor {
        DWORD attributes;
        DWORD dll_name;
        DWORD module_handle;
        DWORD iat;
        DWORD int_table;
        DWORD bound_iat;
        DWORD unload_iat;
        DWORD timestamp;
    };

    const auto* first = reinterpret_cast<const DelayDescriptor*>(base + dir.VirtualAddress);
    const std::size_t count = dir.Size / sizeof(DelayDescriptor);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& desc = first[index];
        if (!desc.dll_name) break;
        if (!(desc.attributes & 1u)) continue;  // modern RVA form only
        if (desc.dll_name >= image || desc.iat >= image || desc.int_table >= image) continue;
        const char* dll_name = reinterpret_cast<const char*>(base + desc.dll_name);
        if (!iequals(dll_name, dll)) continue;

        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc.int_table);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc.iat);
        for (std::size_t thunk = 0;; ++thunk) {
            const auto name_value = names[thunk].u1.AddressOfData;
            if (!name_value) break;
            if (IMAGE_SNAP_BY_ORDINAL64(name_value)) continue;
            if (name_value >= image) break;
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name_value);
            if (symbol == reinterpret_cast<const char*>(import->Name))
                return reinterpret_cast<void**>(&slots[thunk].u1.Function);
        }
    }
    return nullptr;
}

void** find_iat_value(HMODULE module, const void* value) {
    auto* nt = nt_headers(module);
    if (!nt) return nullptr;
    auto* base = reinterpret_cast<std::byte*>(module);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    if (!dir.VirtualAddress || dir.Size < sizeof(void*) ||
        static_cast<std::size_t>(dir.VirtualAddress) + dir.Size > nt->OptionalHeader.SizeOfImage)
        return nullptr;
    auto** first = reinterpret_cast<void**>(base + dir.VirtualAddress);
    const std::size_t count = dir.Size / sizeof(void*);
    for (std::size_t i = 0; i < count; ++i) if (first[i] == value) return first + i;
    return nullptr;
}

std::vector<void**> find_data_pointer_slots(HMODULE module, const void* value) {
    std::vector<void**> out;
    if (!module || !value) return out;
    const auto needle = reinterpret_cast<std::uintptr_t>(value);
    for (const auto& section : sections(module)) {
        // Runtime-resolved imports/dispatch pointers can live in .data or in
        // linker-produced read-only import sections. Scan all non-executable
        // readable image data for the exact ASLR-resolved function address;
        // write_memory() will temporarily change protection only on matches.
        if (!(section.characteristics & IMAGE_SCN_MEM_READ) ||
            (section.characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            section.size < sizeof(void*))
            continue;

        const auto begin = reinterpret_cast<std::uintptr_t>(section.begin);
        const auto end = begin + section.size;
        auto current = (begin + alignof(void*) - 1) & ~(std::uintptr_t(alignof(void*) - 1));
        for (; current + sizeof(void*) <= end; current += sizeof(void*)) {
            std::uintptr_t candidate{};
            std::memcpy(&candidate, reinterpret_cast<const void*>(current), sizeof(candidate));
            if (candidate == needle)
                out.push_back(reinterpret_cast<void**>(current));
        }
    }
    return out;
}

bool write_memory(void* destination, const void* source, std::size_t size) {
    if (!destination || !source || !size) return false;
    DWORD old{};
    if (!VirtualProtect(destination, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(destination, source, size);
    FlushInstructionCache(GetCurrentProcess(), destination, size);
    DWORD ignored{};
    return VirtualProtect(destination, size, old, &ignored) != FALSE;
}

bool write_pointer(void** slot, void* value, void** previous) {
    if (!slot || !*slot) return false;
    if (previous) *previous = *slot;
    return write_memory(slot, &value, sizeof(value));
}
}
