/*
 * Copyright (c) 2020-2022 ndeadly
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "dualsense_controller.hpp"
#include "../mcmitm_config.hpp"
#include <stratosphere.hpp>

namespace ams::controller {

    namespace {

        const constexpr float stick_scale_factor = float(UINT12_MAX) / UINT8_MAX;

        constexpr float accel_scale_factor = 65535 / 16000.0f * 1000;
        constexpr float gyro_scale_factor = 65535 / (13371 * 360.0f) * 1000;

        const uint8_t player_led_flags[] = {
            // Mimic the Switch's player LEDs
            0x01,
            0x03,
            0x0B,
            0x1B,
            0x11,
            0x09,
            0x19,
            0x0A
        };

        const constexpr RGBColour led_disable = {0x00, 0x00, 0x00};

        const RGBColour player_led_colours[] = {
            // Same colours used by PS4
            {0x00, 0x00, 0x40}, // blue
            {0x40, 0x00, 0x00}, // red
            {0x00, 0x40, 0x00}, // green
            {0x20, 0x00, 0x20}, // pink
            // New colours for controllers 5-8
            {0x00, 0x20, 0x20}, // cyan
            {0x30, 0x10, 0x00}, // orange
            {0x20, 0x20, 0x00}, // yellow
            {0x10, 0x00, 0x30}  // purple
        };

    }

    Result DualsenseController::Initialize(void) {
        R_TRY(this->PushRumbleLedState());
        R_TRY(EmulatedSwitchController::Initialize());

        // Request motion calibration data from DualSense
        R_TRY(this->GetCalibrationData(&m_motion_calibration));

        return ams::ResultSuccess();
    }

    Result DualsenseController::SetVibration(const SwitchRumbleData *rumble_data) {
        m_rumble_state.amp_motor_left  = static_cast<uint8_t>(255 * std::max(rumble_data[0].low_band_amp, rumble_data[1].low_band_amp));
        m_rumble_state.amp_motor_right = static_cast<uint8_t>(255 * std::max(rumble_data[0].high_band_amp, rumble_data[1].high_band_amp));
        return this->PushRumbleLedState();
    }

    Result DualsenseController::CancelVibration(void) {
        m_rumble_state.amp_motor_left = 0;
        m_rumble_state.amp_motor_right = 0;
        return this->PushRumbleLedState();
    }

    Result DualsenseController::SetPlayerLed(uint8_t led_mask) {
        uint8_t player_number;
        R_TRY(LedsMaskToPlayerNumber(led_mask, &player_number));
        m_led_flags = player_led_flags[player_number];
        RGBColour colour = player_led_colours[player_number];
        return this->SetLightbarColour(colour);
    }

    Result DualsenseController::SetLightbarColour(RGBColour colour) {
        auto config = mitm::GetGlobalConfig();
        m_led_colour = config->misc.disable_sony_leds ? led_disable : colour;
        return this->PushRumbleLedState();
    }

    void DualsenseController::ProcessInputData(const bluetooth::HidReport *report) {
        auto dualsense_report = reinterpret_cast<const DualsenseReportData *>(&report->data);

        switch(dualsense_report->id) {
            case 0x01:
                this->MapInputReport0x01(dualsense_report); break;
            case 0x31:
                this->MapInputReport0x31(dualsense_report); break;
            default:
                break;
        }
    }

    void DualsenseController::MapInputReport0x01(const DualsenseReportData *src) {
        m_left_stick.SetData(
            static_cast<uint16_t>(stick_scale_factor * src->input0x01.left_stick.x) & 0xfff,
            static_cast<uint16_t>(stick_scale_factor * (UINT8_MAX - src->input0x01.left_stick.y)) & 0xfff
        );
        m_right_stick.SetData(
            static_cast<uint16_t>(stick_scale_factor * src->input0x01.right_stick.x) & 0xfff,
            static_cast<uint16_t>(stick_scale_factor * (UINT8_MAX - src->input0x01.right_stick.y)) & 0xfff
        );

        this->MapButtons(&src->input0x01.buttons);
    }

    void DualsenseController::MapInputReport0x31(const DualsenseReportData *src) {
        m_ext_power = src->input0x31.usb;

        if (!src->input0x31.usb || src->input0x31.full)
            m_charging = false;
        else
            m_charging = true;

        uint8_t battery_level = src->input0x31.battery_level;
        if (!src->input0x31.usb)
            battery_level++;
        if (battery_level > 10)
            battery_level = 10;

        m_battery = static_cast<uint8_t>(8 * (battery_level + 1) / 10) & 0x0e;
    
        m_left_stick.SetData(
            static_cast<uint16_t>(stick_scale_factor * src->input0x31.left_stick.x) & 0xfff,
            static_cast<uint16_t>(stick_scale_factor * (UINT8_MAX - src->input0x31.left_stick.y)) & 0xfff
        );
        m_right_stick.SetData(
            static_cast<uint16_t>(stick_scale_factor * src->input0x31.right_stick.x) & 0xfff,
            static_cast<uint16_t>(stick_scale_factor * (UINT8_MAX - src->input0x31.right_stick.y)) & 0xfff
        );

        this->MapButtons(&src->input0x31.buttons);

        if (m_enable_motion) {
            int16_t acc_x = -static_cast<int16_t>(accel_scale_factor * src->input0x31.acc_z / float(m_motion_calibration.acc.z_max));
            int16_t acc_y = -static_cast<int16_t>(accel_scale_factor * src->input0x31.acc_x / float(m_motion_calibration.acc.x_max));
            int16_t acc_z =  static_cast<int16_t>(accel_scale_factor * src->input0x31.acc_y / float(m_motion_calibration.acc.y_max));

            int16_t vel_x = -static_cast<int16_t>(0.85 * gyro_scale_factor * (src->input0x31.vel_z - m_motion_calibration.gyro.roll_bias)  / ((m_motion_calibration.gyro.roll_max - m_motion_calibration.gyro.roll_bias) / m_motion_calibration.gyro.speed_max));
            int16_t vel_y = -static_cast<int16_t>(0.85 * gyro_scale_factor * (src->input0x31.vel_x - m_motion_calibration.gyro.pitch_bias) / ((m_motion_calibration.gyro.pitch_max - m_motion_calibration.gyro.pitch_bias) / m_motion_calibration.gyro.speed_max));
            int16_t vel_z =  static_cast<int16_t>(0.85 * gyro_scale_factor * (src->input0x31.vel_y - m_motion_calibration.gyro.yaw_bias)   / ((m_motion_calibration.gyro.yaw_max- m_motion_calibration.gyro.yaw_bias) / m_motion_calibration.gyro.speed_max));

            m_motion_data[0].gyro_1  = vel_x;
            m_motion_data[0].gyro_2  = vel_y;
            m_motion_data[0].gyro_3  = vel_z;
            m_motion_data[0].accel_x = acc_x;
            m_motion_data[0].accel_y = acc_y;
            m_motion_data[0].accel_z = acc_z;

            m_motion_data[1].gyro_1  = vel_x;
            m_motion_data[1].gyro_2  = vel_y;
            m_motion_data[1].gyro_3  = vel_z;
            m_motion_data[1].accel_x = acc_x;
            m_motion_data[1].accel_y = acc_y;
            m_motion_data[1].accel_z = acc_z;

            m_motion_data[2].gyro_1  = vel_x;
            m_motion_data[2].gyro_2  = vel_y;
            m_motion_data[2].gyro_3  = vel_z;
            m_motion_data[2].accel_x = acc_x;
            m_motion_data[2].accel_y = acc_y;
            m_motion_data[2].accel_z = acc_z;
        }
        else {
            std::memset(&m_motion_data, 0, sizeof(m_motion_data));
        }
    }

    void DualsenseController::MapButtons(const DualsenseButtonData *buttons) {
        m_buttons.dpad_down   = (buttons->dpad == DualsenseDPad_S)  ||
                                (buttons->dpad == DualsenseDPad_SE) ||
                                (buttons->dpad == DualsenseDPad_SW);
        m_buttons.dpad_up     = (buttons->dpad == DualsenseDPad_N)  ||
                                (buttons->dpad == DualsenseDPad_NE) ||
                                (buttons->dpad == DualsenseDPad_NW);
        m_buttons.dpad_right  = (buttons->dpad == DualsenseDPad_E)  ||
                                (buttons->dpad == DualsenseDPad_NE) ||
                                (buttons->dpad == DualsenseDPad_SE);
        m_buttons.dpad_left   = (buttons->dpad == DualsenseDPad_W)  ||
                                (buttons->dpad == DualsenseDPad_NW) ||
                                (buttons->dpad == DualsenseDPad_SW);

        m_buttons.A = buttons->circle;
        m_buttons.B = buttons->cross;
        m_buttons.X = buttons->triangle;
        m_buttons.Y = buttons->square;

        m_buttons.R  = buttons->R1;
        m_buttons.ZR = buttons->R2;
        m_buttons.L  = buttons->L1;
        m_buttons.ZL = buttons->L2;

        m_buttons.minus = buttons->share;
        m_buttons.plus  = buttons->options;

        m_buttons.lstick_press = buttons->L3;
        m_buttons.rstick_press = buttons->R3;

        m_buttons.capture = buttons->tpad;
        m_buttons.home    = buttons->ps;
    }

    Result DualsenseController::GetCalibrationData(DualsenseImuCalibrationData *calibration) {
        bluetooth::HidReport output;
        R_TRY(this->GetFeatureReport(0x05, &output));

        auto response = reinterpret_cast<DualsenseReportData *>(&output.data);
        std::memcpy(calibration, &response->feature0x05.calibration, sizeof(DualsenseImuCalibrationData));

        return ams::ResultSuccess();
    }

    Result DualsenseController::PushRumbleLedState(void) {
        DualsenseOutputReport0x31 report = {0xa2, 0x31, 0x02, 0x03, 0x14, m_rumble_state.amp_motor_right, m_rumble_state.amp_motor_left};
        report.data[41] = 0x02;
        report.data[44] = 0x02;
        report.data[46] = m_led_flags;
        report.data[47] = m_led_colour.r;
        report.data[48] = m_led_colour.g;
        report.data[49] = m_led_colour.b;
        report.crc = crc32Calculate(report.data, sizeof(report.data));

        m_output_report.size = sizeof(report) - 1;
        std::memcpy(m_output_report.data, &report.data[1], m_output_report.size);

        return this->WriteDataReport(&m_output_report);
    }

}
