// SPDX-License-Identifier: MIT
// QMI8658 motion sensor: tilt angle and shake detection.
#pragma once

// Start the motion task. If the sensor is missing, tilt and shake stay off.
void motion_start(void);
