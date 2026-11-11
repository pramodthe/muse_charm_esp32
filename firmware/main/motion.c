// SPDX-License-Identifier: MIT
// QMI8658 motion sensor: tilt angle, shake detection, and noticing when the
// board is picked up or moved.

#include "motion.h"

#include <math.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse.h"
#include "qmi8658.h"

#ifdef CONFIG_MUSE_MOTION_ENABLE

static const char *TAG = "motion";

#define POLL_MS 20           // 50 Hz; the sensor runs at 250 Hz
#define GRAVITY_SMOOTHING 0.12f
#define SHAKE_SAMPLES 6      // samples above the threshold within ~1 s
#define SHAKE_COOLDOWN_MS 4000
#define MOVE_JOLT 1.2f       // m/s^2 away from rest, twice within a second: handled
#define MOVE_TURN 1.5f       // m/s^2 change in gravity's direction (about 9 degrees): turned
#define QMI8658_REG_RESET 0x60
#define QMI8658_CMD_RESET 0xB0

#ifdef CONFIG_MUSE_LEAN_INVERT
#define LEAN_SIGN -1.0f
#else
#define LEAN_SIGN 1.0f
#endif

static qmi8658_dev_t s_imu;

static bool imu_open(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    const uint16_t candidates[] = {QMI8658_ADDRESS_HIGH, QMI8658_ADDRESS_LOW};
    uint16_t addr = 0;
    for (int i = 0; i < 2 && bus; i++) {
        if (i2c_master_probe(bus, candidates[i], 100) == ESP_OK) {
            addr = candidates[i];
            break;
        }
    }
    if (!addr || qmi8658_init(&s_imu, bus, addr) != ESP_OK) {
        return false;
    }
    // Soft reset first, as in Waveshare's 92_qmi8658_imu example: a warm
    // reboot keeps whatever mode the previous firmware left the sensor in.
    qmi8658_write_register(&s_imu, QMI8658_REG_RESET, QMI8658_CMD_RESET);
    vTaskDelay(pdMS_TO_TICKS(30));
    const bool ok = qmi8658_write_register(&s_imu, QMI8658_CTRL1, 0x60) == ESP_OK &&
                    qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_8G) == ESP_OK &&
                    qmi8658_set_accel_odr(&s_imu, QMI8658_ACCEL_ODR_250HZ) == ESP_OK &&
                    qmi8658_set_gyro_range(&s_imu, QMI8658_GYRO_RANGE_256DPS) == ESP_OK &&
                    qmi8658_set_gyro_odr(&s_imu, QMI8658_GYRO_ODR_250HZ) == ESP_OK &&
                    qmi8658_enable_sensors(&s_imu, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO) == ESP_OK;
    if (ok) {
        qmi8658_set_accel_unit_mps2(&s_imu, true);
        ESP_LOGI(TAG, "QMI8658 at 0x%02x", addr);
    }
    return ok;
}

static void motion_task(void *arg)
{
    (void)arg;
    if (!imu_open()) {
        ESP_LOGW(TAG, "motion sensor not available, tilt and shake are off");
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    const float shake_threshold = CONFIG_MUSE_SHAKE_THRESHOLD_X10 / 10.0f;
    float gx = 0, gy = 0, gz = 0;  // smoothed gravity
    float rx = 0, ry = 0, rz = 0;  // gravity where the board last rested
    bool primed = false;
    int over = 0, window = 0, bumps = 0;
    int64_t cooldown_until = 0;
#ifdef CONFIG_MUSE_DEBUG_LOG
    int64_t next_log = muse_now_ms() + 2000;
#endif
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        float ax = 0, ay = 0, az = 0;
        if (qmi8658_read_accel(&s_imu, &ax, &ay, &az) != ESP_OK || (ax == 0 && ay == 0 && az == 0)) {
            continue;
        }
        if (!primed) {
            gx = rx = ax;
            gy = ry = ay;
            gz = rz = az;
            primed = true;
        }

        // Shake: how far each sample jumps away from the smoothed gravity.
        const float dx = ax - gx, dy = ay - gy, dz = az - gz;
        const float jolt = sqrtf(dx * dx + dy * dy + dz * dz);
        gx += dx * GRAVITY_SMOOTHING;
        gy += dy * GRAVITY_SMOOTHING;
        gz += dz * GRAVITY_SMOOTHING;
        const int64_t now = muse_now_ms();
        if (jolt > shake_threshold) {
            over++;
        }
        if (jolt > MOVE_JOLT) {
            bumps++;
        }
        // Picked up or moved: handled, or gravity points somewhere new.
        const float tx = gx - rx, ty = gy - ry, tz = gz - rz;
        if (bumps >= 2 || tx * tx + ty * ty + tz * tz > MOVE_TURN * MOVE_TURN) {
            g_muse.moved = true;
            bumps = 0;
            rx = gx;
            ry = gy;
            rz = gz;
        }
        if (++window >= 1000 / POLL_MS) {
            window = 0;
            over = 0;
            bumps = 0;
        }
        if (over >= SHAKE_SAMPLES && now > cooldown_until) {
            g_muse.shake = true;
            over = 0;
            cooldown_until = now + SHAKE_COOLDOWN_MS;
        }

        // Tilt: the sensor's y axis runs along the screen's width (as in the
        // stock Gravitysphere app), so its share of gravity gives the roll.
        const float g = sqrtf(gx * gx + gy * gy + gz * gz);
        if (g > 7.0f && g < 12.5f) {
            g_muse.flat = fabsf(gz) / g > 0.85f;
            const float r = muse_clampf(gy / g, -1.0f, 1.0f);
            g_muse.tilt_deg = LEAN_SIGN * asinf(r) * 180.0f / (float)M_PI;
            g_muse.downhill = muse_clampf(-r * 2.5f, -1.0f, 1.0f);
        }

#ifdef CONFIG_MUSE_DEBUG_LOG
        if (now >= next_log) {
            ESP_LOGI(TAG, "accel %.2f %.2f %.2f m/s^2, tilt %.1f deg", ax, ay, az, g_muse.tilt_deg);
            next_log = now + 3000;
        }
#endif
    }
}

#endif  // CONFIG_MUSE_MOTION_ENABLE

void motion_start(void)
{
#ifdef CONFIG_MUSE_MOTION_ENABLE
    xTaskCreatePinnedToCore(motion_task, "muse_motion", 4 * 1024, NULL, 4, NULL, 0);
#endif
}
