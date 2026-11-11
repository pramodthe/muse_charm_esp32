// SPDX-License-Identifier: MIT
// Battery and power, from the AXP2101 on the board's I2C bus. Only reads: the
// chip keeps the settings it powered up with.

#include "power.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "power";

#define AXP2101_ADDRESS 0x34
#define REG_STATUS1 0x00   // bit 5: USB power good, bit 3: battery connected
#define REG_STATUS2 0x01   // bits 6:5: 1 = charging, 2 = discharging
#define REG_CHIP_ID 0x03
#define REG_PERCENT 0xA4   // fuel gauge, 0..100
#define AXP2101_CHIP_ID 0x4A

static i2c_master_dev_handle_t s_dev;

static bool read_reg(uint8_t reg, uint8_t *value)
{
    return s_dev && i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, 100) == ESP_OK;
}

void power_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus || i2c_master_probe(bus, AXP2101_ADDRESS, 100) != ESP_OK) {
        ESP_LOGW(TAG, "no power chip found; battery level is not available");
        return;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDRESS,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return;
    }
    uint8_t id = 0;
    if (!read_reg(REG_CHIP_ID, &id) || id != AXP2101_CHIP_ID) {
        ESP_LOGW(TAG, "the chip at 0x34 is not an AXP2101 (id 0x%02x); battery level is not available", id);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return;
    }
    muse_power_t p;
    if (power_read(&p)) {
        if (p.battery) {
            ESP_LOGI(TAG, "battery %d %%%s%s", p.percent, p.charging ? ", charging" : "", p.usb ? ", USB power" : "");
        } else {
            ESP_LOGI(TAG, "no battery connected%s", p.usb ? ", USB power" : "");
        }
    }
}

bool power_read(muse_power_t *out)
{
    uint8_t s1 = 0, s2 = 0, percent = 0;
    *out = (muse_power_t){0};
    if (!read_reg(REG_STATUS1, &s1) || !read_reg(REG_STATUS2, &s2) || !read_reg(REG_PERCENT, &percent)) {
        return false;
    }
    out->known = true;
    out->usb = (s1 & 0x20) != 0;
    out->battery = (s1 & 0x08) != 0;
    out->charging = ((s2 >> 5) & 0x03) == 1;
    out->percent = percent > 100 ? 100 : percent;
    return true;
}
