/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Ha Thach for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "tusb.h"
#include "uf2.h"

#if CFG_TUSB_MCU == OPT_MCU_ESP32S2
#include "esp_partition.h"
#include "esp_ota_ops.h"
#endif

/*------------------------------------------------------------------*/
/* MACRO TYPEDEF CONSTANT ENUM
 *------------------------------------------------------------------*/

static WriteState _wr_state = { 0 };

//--------------------------------------------------------------------+
// tinyusb callbacks
//--------------------------------------------------------------------+

// Invoked when received SCSI_CMD_INQUIRY
// Application fill vendor id, product id and revision with string up to 8, 16, 4 characters respectively
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
  (void) lun;

  const char vid[] = "Adafruit";
  const char pid[] = "ESP32S UF2";
  const char rev[] = "1.0";

  memcpy(vendor_id  , vid, strlen(vid));
  memcpy(product_id , pid, strlen(pid));
  memcpy(product_rev, rev, strlen(rev));
}

// Invoked when received Test Unit Ready command.
// return true allowing host to read/write this LUN e.g SD card inserted
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
  (void) lun;
  return true;
}

// Callback invoked when received an SCSI command not in built-in list below
// - READ_CAPACITY10, READ_FORMAT_CAPACITY, INQUIRY, MODE_SENSE6, REQUEST_SENSE
// - READ10 and WRITE10 has their own callbacks
int32_t tud_msc_scsi_cb (uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize)
{
  void const* response = NULL;
  uint16_t resplen = 0;

  // most scsi handled is input
  bool in_xfer = true;

  switch (scsi_cmd[0])
  {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
      // Host is about to read/write etc ... better not to disconnect disk
      resplen = 0;
    break;

    default:
      // Set Sense = Invalid Command Operation
      tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);

      // negative means error -> tinyusb could stall and/or response with failed status
      resplen = -1;
    break;
  }

  // return resplen must not larger than bufsize
  if ( resplen > bufsize ) resplen = bufsize;

  if ( response && (resplen > 0) )
  {
    if(in_xfer)
    {
      memcpy(buffer, response, resplen);
    }else
    {
      // SCSI output
    }
  }

  return resplen;
}

// Callback invoked when received READ10 command.
// Copy disk's data to buffer (up to bufsize) and return number of copied bytes.
int32_t tud_msc_read10_cb (uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
{
  (void) lun;
  memset(buffer, 0, bufsize);

  // since we return block size each, offset should always be zero
  TU_ASSERT(offset == 0, -1);

  uint32_t count = 0;

  while ( count < bufsize )
  {
    uf2_read_block(lba, buffer);

    lba++;
    buffer += 512;
    count  += 512;
  }

  return count;
}

// Callback invoked when received WRITE10 command.
// Process data in buffer to disk's storage and return number of written bytes
int32_t tud_msc_write10_cb (uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize)
{
  (void) lun;
  (void) offset;

  uint32_t count = 0;
  while ( count < bufsize )
  {
    // Consider non-uf2 block write as successful
    // only break if write_block is busy with flashing (return 0)
    if ( 0 == uf2_write_block(lba, buffer, &_wr_state) ) break;

    lba++;
    buffer += 512;
    count  += 512;
  }

  return count;
}

// Callback invoked when WRITE10 command is completed (status received and accepted by host).
void tud_msc_write10_complete_cb(uint8_t lun)
{
  (void) lun;
  static bool first_write = true;

  // abort the DFU, uf2 block failed integrity check
  if ( _wr_state.aborted )
  {
    // aborted and reset
    board_led_state(STATE_WRITING_FINISHED);
  }
  else if ( _wr_state.numBlocks )
  {
    // Start LED writing pattern with first write
    if (first_write)
    {
      first_write = false;
      board_led_state(STATE_WRITING_STARTED);
    }

    // All block of uf2 file is complete --> complete DFU process
    if (_wr_state.numWritten >= _wr_state.numBlocks)
    {
      board_led_state(STATE_WRITING_FINISHED);

#if CFG_TUSB_MCU == OPT_MCU_ESP32S2
      // Set partition OTA0 as bootable
      esp_ota_set_boot_partition(esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL));
      esp_restart();
#endif

    }
  }
}

// Invoked when received SCSI_CMD_READ_CAPACITY_10 and SCSI_CMD_READ_FORMAT_CAPACITY to determine the disk size
// Application update block count and block size
void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size)
{
  (void) lun;

  *block_count = CFG_UF2_NUM_BLOCKS;
  *block_size  = 512;
}

// Invoked when received Start Stop Unit command
// - Start = 0 : stopped power mode, if load_eject = 1 : unload disk storage
// - Start = 1 : active mode, if load_eject = 1 : load disk storage
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
  (void) lun;
  (void) power_condition;

  if ( load_eject )
  {
    if (start)
    {
      // load disk storage
    }else
    {
      // unload disk storage
    }
  }

  return true;
}