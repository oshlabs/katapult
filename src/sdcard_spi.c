// SPI SDCard Commands
//
// Copyright (C) 2022 Eric Callahan <arksine.code@gmail.com>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <string.h>         // memmove
#include "autoconf.h"       // CONFIG_SD_SPI_CS
#include "sdcard.h"         // sdcard_init
#include "board/gpio.h"     // gpio_out_setup
#include "board/misc.h"     // timer_read_time
#include "board/io.h"       // readb
#include "sched.h"          // udelay

DECL_CTR("DECL_SD_SPI_CS_PIN " __stringify(CONFIG_SD_SPI_CS_PIN));
extern uint32_t sdcard_cs_gpio; // Generated by buildcommands.py

#define CRC7_POLY           0x12
#define CRC16_POLY          0x1021
#define SPI_INIT_RATE       400000
#define SPI_XFER_RATE       4000000

static struct {
    struct spi_config config;
    struct gpio_out cs_pin;
    uint8_t flags;
    uint8_t err;
} sd_spi;

enum {SDF_INITIALIZED = 1, SDF_HIGH_CAPACITY = 2, SDF_WRITE_PROTECTED = 4,
      SDF_DEINIT =  8};
enum {SDE_NO_IDLE = 1, SDE_IF_COND_ERR = 2, SDE_CRC_ERR = 4,
      SDE_OP_COND_ERR = 8, SDE_OCR_ERR = 16, SDE_READ_ERR = 32,
      SDE_WRITE_ERR = 64, SDE_OTHER_ERR = 128};
// COMMAND FLAGS
enum {CF_APP_CMD = 1, CF_FULL_RESP = 2, CF_NOT_EXPECT = 4};

/**********************************************************
 *
 * CRC Helper Methods
 *
 * ********************************************************/

// Standard CRC
static uint8_t
calc_crc7(uint8_t *data, uint16_t length)
{
    uint8_t crc = 0;
    while (length--)
    {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ CRC7_POLY;
            else
                crc = crc << 1;
        }
    }
    return crc | 1;
}

static uint16_t
calc_crc16(uint8_t *data, uint16_t length)
{
    uint16_t crc = 0;
    while (length--)
    {
        crc ^= (*data++ << 8);
        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ CRC16_POLY;
            else
                crc = crc << 1;
        }
    }
    return crc;
}

/**********************************************************
 *
 * SD Card Commands
 *
 * ********************************************************/
static void
populate_buffer(uint8_t command, uint32_t arg, uint8_t* buf)
{
    buf[0] = command | 0x40;
    buf[1] = (arg >> 24) & 0xFF;
    buf[2] = (arg >> 16) & 0xFF;
    buf[3] = (arg >> 8) & 0xFF;
    buf[4] = (arg & 0xFF);
    uint8_t crc = calc_crc7(buf, 5);
    buf[5] = crc;
}

static uint8_t
send_command(uint8_t command, uint32_t arg, uint8_t* buf, uint8_t cmd_flags)
{
    uint8_t ret;
    spi_prepare(sd_spi.config);
    gpio_out_write(sd_spi.cs_pin, 0);
    memset(buf, 0, 8);
    if (cmd_flags & CF_APP_CMD) {
        buf[0] = SDCMD_APP_CMD | 0x40;
        uint8_t crc = calc_crc7(buf, 5);
        buf[5] = crc;
        spi_transfer(sd_spi.config, 0, 6, buf);
    }
    populate_buffer(command, arg, buf);
    spi_transfer(sd_spi.config, 0, 6, buf);
    memset(buf, 0xFF, 8);
    spi_transfer(sd_spi.config, 1, 8, buf);
    for (uint8_t i = 0; i < 8; i++) {
        ret = readb(&buf[i]);
        if (ret != 0xFF) {
            if (i) {
                uint8_t recd = 8 - i;
                memmove(buf, &buf[i], recd);
                if (cmd_flags & CF_FULL_RESP) {
                    // need to read out a complete response
                    uint8_t rem = 8 - recd;
                    memset(&buf[recd], 0xFF, rem);
                    spi_transfer(sd_spi.config, 1, rem, &buf[recd]);
                }
            }
            break;
        }
    }
    gpio_out_write(sd_spi.cs_pin, 1);
    return ret;
}

static uint8_t
check_command(uint8_t cmd, uint32_t arg, uint8_t* buf, uint8_t flags,
              uint8_t expect, uint8_t attempts)
{
    while (attempts) {
        uint8_t ret = send_command(cmd, arg, buf, flags);
        if (flags & CF_NOT_EXPECT) {
            if (ret != expect)
                return 1;
        }
        else if (ret == expect)
            return 1;
        attempts--;
        if (attempts)
            udelay(1000);
    }
    return 0;
}

static uint8_t
find_token(uint8_t token, uint32_t timeout_us)
{
    uint8_t buf[1];
    uint32_t endtime = timer_read_time() + timer_from_us(timeout_us);
    while(timer_is_before(timer_read_time(), endtime))
    {
        writeb(buf, 0xFF);
        spi_transfer(sd_spi.config, 1, 1, buf);
        if (readb(buf) == token) {
            return 1;
        }
    }
    return 0;
}

uint8_t
sdcard_write_sector(uint8_t *buf, uint32_t sector)
{
    if (!(sd_spi.flags & SDF_INITIALIZED))
        return -1;
    uint32_t offset = sector;
    if (!(sd_spi.flags & SDF_HIGH_CAPACITY))
        offset = sector * SD_SECTOR_SIZE;
    uint16_t crc = calc_crc16(buf, SD_SECTOR_SIZE);
    uint8_t cmd_buf[8];
    uint8_t ret = 1;
    populate_buffer(SDCMD_WRITE_BLOCK, offset, cmd_buf);
    spi_prepare(sd_spi.config);
    gpio_out_write(sd_spi.cs_pin, 0);
    spi_transfer(sd_spi.config, 0, 6, cmd_buf);
    memset(cmd_buf, 0xFF, 8);
    spi_transfer(sd_spi.config, 1, 8, cmd_buf);
    for (uint8_t i = 0; i < 8; i++) {
        ret = readb(&cmd_buf[i]);
        if (ret != 0xFF)
            break;
    }
    if (ret == 0xFF) {
        gpio_out_write(sd_spi.cs_pin, 1);
        sd_spi.err |= SDE_WRITE_ERR;
        return 0;
    }
    // Send Header
    cmd_buf[0] = 0xFE;
    spi_transfer(sd_spi.config, 0, 1, cmd_buf);
    // Send Data
    spi_transfer(sd_spi.config, 0, SD_SECTOR_SIZE, buf);
    // Send CRC
    cmd_buf[0] = (crc >> 8) & 0xFF;
    cmd_buf[1] = crc & 0xFF;
    spi_transfer(sd_spi.config, 0, 2, cmd_buf);
    // Check for successful response
    memset(cmd_buf, 0xFF, 8);
    spi_transfer(sd_spi.config, 1, 8, cmd_buf);
    // Check the response
    for (uint8_t i = 0; i < 8; i ++)
    {
        ret = readb(&cmd_buf[i]);
        if (ret != 0xFF)
            break;
    }
    if ((ret & 0x1F) == 5)
        ret = 0;
    else
        ret = 2;
    // Wait for card to come out of busy state, 50ms timeout
    if (!find_token(0xFF, 50000))
        ret += 1;
    gpio_out_write(sd_spi.cs_pin, 1);
    if (ret > 0) {
        sd_spi.err |= SDE_WRITE_ERR;
        return 0;
    }
    return 1;
}

static uint8_t
read_data_block(uint8_t cmd, uint32_t arg, uint8_t* buf, uint32_t length)
{
    uint8_t ret = 0;
    uint8_t cmd_buf[8];
    populate_buffer(cmd, arg, cmd_buf);
    spi_prepare(sd_spi.config);
    gpio_out_write(sd_spi.cs_pin, 0);
    spi_transfer(sd_spi.config, 0, 6, cmd_buf);
    // find the first non-zero response, maximum of 16 tries
    for (uint8_t i = 0; i < 16; i++) {
        writeb(cmd_buf, 0xFF);
        spi_transfer(sd_spi.config, 1, 1, cmd_buf);
        ret = readb(cmd_buf);
        if (ret != 0xFF)
            break;
    }
    if (ret != 0)
        // Invalid Response
        ret = 1;

    // Find Transfer Start token, 50ms timeout
    if (!find_token(0xFE, 50000))
        ret = 2;

    // Read out the response.  This is done regardless
    // of success to make sure the sdcard response is
    // flushed
    memset(buf, 0xFF, length);
    spi_transfer(sd_spi.config, 1, length, buf);
    // Read and check crc
    memset(cmd_buf, 0xFF, 8);
    spi_transfer(sd_spi.config, 1, 2, cmd_buf);
    // Exit busy state
    find_token(0xFF, 50000);
    gpio_out_write(sd_spi.cs_pin, 1);
    uint16_t recd_crc = (cmd_buf[0] << 8) | cmd_buf[1];
    uint16_t crc = calc_crc16(buf, length);
    if (recd_crc != crc) {
        sd_spi.err |= SDE_CRC_ERR;
        ret = 3;
    }
    if (ret > 0) {
        sd_spi.err |= SDE_READ_ERR;
        return 0;
    }
    return 1;
}

uint8_t
sdcard_read_sector(uint8_t *buf, uint32_t sector)
{
    if (!(sd_spi.flags & SDF_INITIALIZED))
        return -1;
    uint32_t offset = sector;
    if (!(sd_spi.flags & SDF_HIGH_CAPACITY))
        offset = sector * SD_SECTOR_SIZE;
    return read_data_block(
        SDCMD_READ_SINGLE_BLOCK, offset, buf, SD_SECTOR_SIZE
    );
}

static uint8_t
sdcard_check_write_protect(void)
{
    // Read the CSD register to check the write protect
    // bit.  A name change can't be performed on a
    // write protected card.
    uint8_t csd_buf[16];
    if (!read_data_block(SDCMD_SEND_CSD, 0, csd_buf, 16))
        return 0;
    if ((csd_buf[14] & 0x30) != 0)
        // card is write protected
        return 0;
    return 1;
}

uint8_t
sdcard_init(void)
{
    sd_spi.cs_pin = gpio_out_setup(sdcard_cs_gpio, 1);
    sd_spi.config = spi_setup(CONFIG_SD_SPI_BUS, 0, SPI_INIT_RATE);
    // per the spec, delay for 1ms and apply a minimum of 74 clocks
    // with CS high
    udelay(1000);
    spi_prepare(sd_spi.config);
    uint8_t buf[8];
    memset(buf, 0xFF, 8);
    for (uint8_t i = 0; i < 10; i++) {
        spi_transfer(sd_spi.config, 0, 8, buf);
    }
    // attempt to go idle
    if (!check_command(SDCMD_GO_IDLE_STATE, 0, buf, 0, 1, 50)) {
        sd_spi.err |= SDE_NO_IDLE;
        return 0;
    }
    // Check SD Card Version
    uint8_t sd_ver = 0;
    if (!check_command(SDCMD_SEND_IF_COND, 0x10A, buf,
                       CF_FULL_RESP | CF_NOT_EXPECT, 0xFF, 3))
    {
        sd_spi.err |= SDE_IF_COND_ERR;
        return 0;
    }
    if (buf[0] & 4)
        sd_ver = 1;
    else if (buf[0] == 1 && buf[3] == 1 && buf[4] == 10)
        sd_ver = 2;
    else {
        sd_spi.err |= SDE_IF_COND_ERR;
        return 0;
    }
    // Enable CRC Checks
    if (!check_command(SDCMD_CRC_ON_OFF, 1, buf, 0, 1, 3)) {
        sd_spi.err |= SDE_CRC_ERR;
        return 0;
    }

    // Read OCR Register to determine if voltage is acceptable
    if (!check_command(SDCMD_READ_OCR, 0, buf, CF_FULL_RESP, 1, 20)) {
        sd_spi.err |= SDE_OCR_ERR;
        return 0;
    }

    if ((buf[2] & 0x30) != 0x30) {
        // Voltage between 3.2-3.4v not supported by this
        // card
        sd_spi.err |= SDE_OCR_ERR;
        return 0;

    }

    // Finsh init and come out of idle.  This can take some time,
    // give up to 250 attempts
    if (!check_command(SDCMD_SEND_OP_COND, (sd_ver == 1) ? 0 : (1 << 30),
                       buf, CF_APP_CMD, 0, 250))
    {
        sd_spi.err |= SDE_OP_COND_ERR;
        return 0;
    }

    if (sd_ver == 2) {
        // read OCR again to determine capacity
        if (!check_command(SDCMD_READ_OCR, 0, buf, CF_FULL_RESP, 0, 5)) {
            sd_spi.err |= SDE_OCR_ERR;
            return 0;
        }
        if (buf[1] & 0x40)
            sd_spi.flags |= SDF_HIGH_CAPACITY;
    }

    if (!check_command(SDCMD_SET_BLOCKLEN, SD_SECTOR_SIZE, buf, 0, 0, 3)) {
        sd_spi.err |= SDE_OTHER_ERR;
        return 0;
    }
    if (!sdcard_check_write_protect()) {
        sd_spi.flags |= SDF_WRITE_PROTECTED;
        return 0;
    }
    sd_spi.flags |= SDF_INITIALIZED;
    spi_set_rate(&(sd_spi.config), SPI_XFER_RATE);
    return 1;
}

void
sdcard_deinit(void)
{
    if (sd_spi.flags & SDF_DEINIT)
        return;
    sd_spi.flags |= SDF_DEINIT;
    uint8_t buf[8];
    // return to idle and disable crc checks
    send_command(SDCMD_GO_IDLE_STATE, 0, buf, 0);
    send_command(SDCMD_CRC_ON_OFF, 0, buf, 0);
}