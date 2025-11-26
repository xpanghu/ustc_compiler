#include "util.hpp"
#include <cstdint>

std::string ptr_to_str(const void* ptr)
{
    const char* translate = "0123456789abcdef";

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

    uintptr_t a = addr & 0xF;
    uintptr_t b = (addr >> 4) & 0xF;
    uintptr_t c = (addr >> 8) & 0xF;
    uintptr_t d = (addr >> 12) & 0xF;

    return {'<', translate[d], translate[c], translate[b], translate[a], '>'};
}
