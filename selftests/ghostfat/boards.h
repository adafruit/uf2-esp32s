#ifndef GHOSTFAT_TEST_CONFIG_H
#define GHOSTFAT_TEST_CONFIG_H

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// From board_api.h

#define BOARD_FLASH_APP_START  0
#define TINYUF2_LED 0
#define TINYUF2_DFU_DOUBLE_TAP      0
#define TINYUF2_DISPLAY 0

const uint32_t 4MiB_FLASH_SIZE = 4u * 1024u * 1024u;

#endif  // GHOSTFAT_TEST_CONFIG_H