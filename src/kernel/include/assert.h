#ifndef WASMOS_ASSERT_H
#define WASMOS_ASSERT_H

#include <stdlib.h>

#define assert(cond) do { if (!(cond)) { abort(); } } while (0)

#endif
