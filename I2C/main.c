/***************************************************************************//**
 * @file main.c
 * @brief main() function.
 *******************************************************************************
 * # License
 * <b>Copyright 2020 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc. Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement. This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/
#include "sl_component_catalog.h"
#include "sl_system_init.h"
#include "app.h"
#if defined(SL_CATALOG_POWER_MANAGER_PRESENT)
#include "sl_power_manager.h"
#endif
#if defined(SL_CATALOG_KERNEL_PRESENT)
#include "sl_system_kernel.h"
#else // SL_CATALOG_KERNEL_PRESENT
#include "sl_system_process_action.h"
#endif // SL_CATALOG_KERNEL_PRESENT

#include "app_log.h"
#include "em_i2c.h"
#include "sl_i2cspm_instances.h"
#include "sl_sleeptimer.h"

// SI7021_Config_Settings Si7021 Configuration Settings
#define SI7021_I2C_DEVICE                (sl_i2cspm_sensor) /**< I2C device used to control the Si7021  */
#define SI7021_I2C_BUS_ADDRESS           0x40               /**< I2C bus address                        */
#define SI7021_DEVICE_ID                 0x15               /**< Si7021 device ID                       */

// Si7021 command macro definitions
#define SI7021_CMD_MEASURE_RH            0xE5               /**< Measure Relative Humidity, Hold Master Mode */
#define SI7021_CMD_MEASURE_RH_NO_HOLD    0xF5               /**< Measure Relative Humidity, No Hold Master Mode */
#define SI7021_CMD_MEASURE_TEMP          0xE3               /**< Measure Temperature, Hold Master Mode */
#define SI7021_CMD_MEASURE_TEMP_NO_HOLD  0xF3               /**< Measure Temperature, No Hold Master Mode */
#define SI7021_CMD_READ_TEMP             0xE0               /**< Read Temperature Value from Previous RH Measurement */
#define SI7021_CMD_RESET                 0xFE               /**< Reset */
#define SI7021_CMD_WRITE_USER_REG1       0xE6               /**< Write RH/T User Register 1 */
#define SI7021_CMD_READ_USER_REG1        0xE7               /**< Read RH/T User Register 1 */
#define SI7021_CMD_WRITE_HEATER_CTRL     0x51               /**< Write Heater Control Register */
#define SI7021_CMD_READ_HEATER_CTRL      0x11               /**< Read Heater Control Register */
#define SI7021_CMD_READ_ID_BYTE1         { 0xFA, 0x0F }       /**< Read Electronic ID 1st Byte */
#define SI7021_CMD_READ_ID_BYTE2         { 0xFC, 0xC9 }       /**< Read Electronic ID 2nd Byte */
#define SI7021_CMD_READ_FW_REV           { 0x84, 0xB8 }       /**< Read Firmware Revision */

uint32_t decode_rh(uint8_t* read_register)
{
  uint32_t rhValue;

  // Formula to decode read RH from the Si7021 Datasheet
  rhValue = ((uint32_t) read_register[0] << 8) + (read_register[1] & 0xfe);
  rhValue = (((rhValue) * 125) >> 16) - 6;

  return rhValue;
}

uint8_t crc8(uint8_t *data, uint8_t length) {
    uint8_t crc = 0x00;

    for (uint8_t i = 0; i < length; i++) {
        crc ^= data[i];

        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static I2C_TransferReturn_TypeDef SI7021_transaction(uint16_t flag,
                                                     uint8_t *writeCmd,
                                                     uint16_t writeLen,
                                                     uint8_t *readCmd,
                                                     uint16_t readLen)
{
  I2C_TransferSeq_TypeDef seq;
  I2C_TransferReturn_TypeDef ret;

  seq.addr = SI7021_I2C_BUS_ADDRESS << 1;
  seq.flags = flag;

  switch (flag) {
    // Send the write command from writeCmd
    case I2C_FLAG_WRITE:
      seq.buf[0].data = writeCmd;
      seq.buf[0].len  = writeLen;

      break;

    // Receive data into readCmd of readLen
    case I2C_FLAG_READ:
      seq.buf[0].data = readCmd;
      seq.buf[0].len  = readLen;

      break;

    // Send the write command from writeCmd
    // and receive data into readCmd of readLen
    case I2C_FLAG_WRITE_READ:
      seq.buf[0].data = writeCmd;
      seq.buf[0].len  = writeLen;

      seq.buf[1].data = readCmd;
      seq.buf[1].len  = readLen;

      break;

    default:
      return i2cTransferUsageFault;
  }

  // Perform the transfer and return status from the transfer
  ret = I2CSPM_Transfer(SI7021_I2C_DEVICE, &seq);

  return ret;
}

static void SI7021_measure(uint32_t *rhData )
{
  I2C_TransferReturn_TypeDef ret;
  uint8_t cmd;
  uint8_t readData[3];
  uint32_t timeout;

  // Start no-hold measurement by writing command to si7021
  cmd = SI7021_CMD_MEASURE_RH_NO_HOLD;
  ret = SI7021_transaction(I2C_FLAG_WRITE, &cmd, 1, 0, 0);

  // Wait for data to become ready
  timeout = 300;
  while (timeout--) {
    ret = SI7021_transaction(I2C_FLAG_READ, 0, 0, readData, 3);
    if (ret == i2cTransferDone) {
      break;
    } else if (ret == i2cTransferNack) {
      // Si7021 returns a Nack if data not ready
      continue;
    }
  }

  // comment the following code
  uint8_t check_crc = crc8( readData, 2 );

  if (check_crc == readData[2]) {
      app_log("CRC HM measure is true\r\n");
  } else {
      app_log("CRC FAILED\r\n");
  }

  // Data is ready, decode the RH value
  *rhData = decode_rh(readData);
}


int main(void)
{
  uint32_t relative_humidity = 0;

  // Initialize Silicon Labs device, system, service(s) and protocol stack(s).
  // Note that if the kernel is present, processing task(s) will be created by
  // this call.
  sl_system_init();

  // Initialize the application. For example, create periodic timer(s) or
  // task(s) if the kernel is present.
  app_init();

#if defined(SL_CATALOG_KERNEL_PRESENT)
  // Start the kernel. Task(s) created in app_init() will start running.
  sl_system_kernel_start();
#else // SL_CATALOG_KERNEL_PRESENT
  while (1) {
    // Do not remove this call: Silicon Labs components process action routine
    // must be called from the super loop.
    sl_system_process_action();

    // Application process.
    app_process_action();

    SI7021_measure( &relative_humidity );

    app_log_info("Relative Humidity = %ld%%\r\n", relative_humidity);

    sl_sleeptimer_delay_millisecond(8000);

#if defined(SL_CATALOG_POWER_MANAGER_PRESENT)
    // Let the CPU go to sleep if the system allows it.
    sl_power_manager_sleep();
#endif
  }
#endif // SL_CATALOG_KERNEL_PRESENT
}
