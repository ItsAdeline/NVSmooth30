#pragma once
#include "common.hpp"

namespace nvs30::fatbin {
struct RewriteStats {
    std::uint32_t entries{};
    std::uint32_t cubins{};
    std::uint32_t sm89_to_sm86{};
    std::uint32_t sm120_left{};
    std::uint32_t elf_headers{};
};

struct RewrittenImage {
    std::vector<std::byte> bytes;
    RewriteStats stats;
    bool valid{};
};

RewrittenImage rewrite_sm89_to_sm86(const void* image);
}

