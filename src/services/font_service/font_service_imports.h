#ifndef WASMOS_FONT_SERVICE_IMPORTS_H
#define WASMOS_FONT_SERVICE_IMPORTS_H

#include "../../drivers/include/wasmos_native_driver.h"
#include "../../drivers/include/wasmos_driver_abi.h"
#include "../../../lib/libc/include/wasmos/font_ipc.h"
void wasmos_stbtt_alloc_reset(void);
#include "../../../libs/stb/stb_truetype.h"

#endif
