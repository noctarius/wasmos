#pragma once

#include <stdint.h>

int kpanic_symbolize(uint64_t addr, const char** name, uint64_t* sym_addr);
