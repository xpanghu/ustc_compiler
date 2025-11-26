#include "Register.hpp"
#include <string>

std::string Reg::print() const {
    if (id == 0) {
        return "$zero";
    }
    if (id == 1) {
        return "$ra";
    }
    if (id == 2) {
        return "$tp";
    }
    if (id == 3) {
        return "$sp";
    }
    if (4 <= id and id <= 11) {
        return "$a" + std::to_string(id - 4);
    }
    if (12 <= id and id <= 20) {
        return "$t" + std::to_string(id - 12);
    }
    if (id == 22) {
        return "$fp";
    }
    assert(false);
}

std::string Reg::safe_print() const
{
    if (id == 0) {
        return "$zero";
    }
    if (id == 1) {
        return "$ra";
    }
    if (id == 2) {
        return "$tp";
    }
    if (id == 3) {
        return "$sp";
    }
    if (4 <= id and id <= 11) {
        return "$a" + std::to_string(id - 4);
    }
    if (12 <= id and id <= 20) {
        return "$t" + std::to_string(id - 12);
    }
    if (id == 22) {
        return "$fp";
    }
    return "<error id " + std::to_string(id) + ">";
}

std::string FReg::print() const {

    if (id <= 7) {
        return "$fa" + std::to_string(id);
    }
    if (8 <= id and id <= 23) {
        return "$ft" + std::to_string(id - 8);
    }
    if (24 <= id and id <= 31) {
        return "$fs" + std::to_string(id - 24);
    }
    assert(false);
}

std::string FReg::safe_print() const
{
    if (id <= 7) {
        return "$fa" + std::to_string(id);
    }
    if (8 <= id and id <= 23) {
        return "$ft" + std::to_string(id - 8);
    }
    if (24 <= id and id <= 31) {
        return "$fs" + std::to_string(id - 24);
    }
    return "<error id " + std::to_string(id) + ">";
}

std::string CFReg::safe_print() const
{
    return "$fcc" + std::to_string(id);
}
