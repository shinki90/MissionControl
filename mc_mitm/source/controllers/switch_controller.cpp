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
#include "switch_controller.hpp"
#include "../utils.hpp"
#include "../mcmitm_config.hpp"
#include <string>

namespace ams::controller {

    namespace {

        const uint8_t led_player_mappings[] = {
            SwitchPlayerNumber_Unknown, //0000
            SwitchPlayerNumber_One,     //0001
            SwitchPlayerNumber_Unknown, //0010
            SwitchPlayerNumber_Two,     //0011
            SwitchPlayerNumber_Unknown, //0100
            SwitchPlayerNumber_Six,     //0101
            SwitchPlayerNumber_Eight,   //0110
            SwitchPlayerNumber_Three,   //0111
            SwitchPlayerNumber_One,     //1000
            SwitchPlayerNumber_Five,    //1001
            SwitchPlayerNumber_Six,     //1010
            SwitchPlayerNumber_Seven,   //1011
            SwitchPlayerNumber_Two,     //1100
            SwitchPlayerNumber_Seven,   //1101
            SwitchPlayerNumber_Three,   //1110
            SwitchPlayerNumber_Four,    //1111
        };

    }

    Result LedsMaskToPlayerNumber(uint8_t led_mask, uint8_t *player_number) {
        *player_number = led_player_mappings[led_mask & 0xf];
        if (*player_number == SwitchPlayerNumber_Unknown)
            return -1;

        return ams::ResultSuccess();
    }

    std::string GetControllerDirectory(const bluetooth::Address *address) {
        char path[0x100];
        util::SNPrintf(path, sizeof(path), "sdmc:/config/MissionControl/controllers/%02x%02x%02x%02x%02x%02x",
            address->address[0],
            address->address[1],
            address->address[2],
            address->address[3],
            address->address[4],
            address->address[5]
        );
        return path;
    }

    Result SwitchController::Initialize(void) {
        if (this->HasSetTsiDisableFlag())
            m_settsi_supported = false;

        return ams::ResultSuccess(); 
    }

    bool SwitchController::HasSetTsiDisableFlag(void) {
        std::string flag_file = GetControllerDirectory(&m_address) + "/settsi_disable.flag";

        bool file_exists;
        if (R_SUCCEEDED(fs::HasFile(&file_exists, flag_file.c_str()))) {
            return file_exists;
        }

        return false;
    }

    Result SwitchController::HandleDataReportEvent(const bluetooth::HidReportEventInfo *event_info) {
        const bluetooth::HidReport *report;
        if (hos::GetVersion() >= hos::Version_9_0_0) {
            report = &event_info->data_report.v9.report;
        } else if (hos::GetVersion() >= hos::Version_7_0_0) {
            report = reinterpret_cast<const bluetooth::HidReport *>(&event_info->data_report.v7.report);
        } else {
            report = reinterpret_cast<const bluetooth::HidReport *>(&event_info->data_report.v1.report);
        }

        if (!m_future_responses.empty()) {
            if ((m_future_responses.back()->GetType() == BtdrvHidEventType_Data) && (m_future_responses.back()->GetUserData() == report->data[0])) {
                m_future_responses.back()->SetData(*event_info);
            }
        }

        this->UpdateControllerState(report);

        auto switch_report = reinterpret_cast<SwitchReportData *>(m_input_report.data);
        switch (switch_report->id) {
            case 0x21:
                this->ApplyButtonCombos(&switch_report->input0x21.buttons); 

	            if (switch_report->input0x21.response.id == SubCmd_SpiFlashRead) {
	                if (switch_report->input0x21.response.data.spi_flash_read.address == 0x6050) {
	                    if (ams::mitm::GetSystemLanguage() == 10) {
	                        uint8_t data[] = {0xff, 0xd7, 0x00, 0x00, 0x57, 0xb7, 0x00, 0x57, 0xb7, 0x00, 0x57, 0xb7};
	                        std::memcpy(switch_report->input0x21.response.data.spi_flash_read.data, data, sizeof(data));
	                    }
	                }
	            }
				break;
            case 0x30:
                this->ApplyButtonCombos(&switch_report->input0x30.buttons); break;
            case 0x31:
            case 0x32:
            case 0x33:
            case 0x3f:
            default:
                break;
        }

        return bluetooth::hid::report::WriteHidDataReport(m_address, &m_input_report);
    }

    Result SwitchController::HandleSetReportEvent(const bluetooth::HidReportEventInfo *event_info) {
        if (!m_future_responses.empty()) {
            if (m_future_responses.back()->GetType() == BtdrvHidEventType_SetReport) {
                m_future_responses.back()->SetData(*event_info);
            }
            return ams::ResultSuccess();
        }

        return bluetooth::hid::report::WriteHidSetReport(m_address, event_info->set_report.res);
    }

    Result SwitchController::HandleGetReportEvent(const bluetooth::HidReportEventInfo *event_info) {
        if (!m_future_responses.empty()) {
            if (m_future_responses.back()->GetType() == BtdrvHidEventType_GetReport) {
                m_future_responses.back()->SetData(*event_info);
            }
            return ams::ResultSuccess();
        }

        auto report = hos::GetVersion() >= hos::Version_9_0_0 ? &event_info->get_report.v9.report : reinterpret_cast<const bluetooth::HidReport *>(&event_info->get_report.v1.report);
        return bluetooth::hid::report::WriteHidGetReport(m_address, report);
    }

    Result SwitchController::HandleOutputDataReport(const bluetooth::HidReport *report) {
        return this->WriteDataReport(report);
    }

    Result SwitchController::WriteDataReport(const bluetooth::HidReport *report) {
        return btdrvWriteHidData(m_address, report);
    }

    Result SwitchController::WriteDataReport(const bluetooth::HidReport *report, uint8_t response_id, bluetooth::HidReport *out_report) {       
        auto response = std::make_shared<HidResponse>(BtdrvHidEventType_Data);
        response->SetUserData(response_id);
        m_future_responses.push(response);
        ON_SCOPE_EXIT { m_future_responses.pop(); };

        R_TRY(btdrvWriteHidData(m_address, report));

        if (!response->TimedWait(ams::TimeSpan::FromMilliSeconds(500))) {
            return -1; // This should return a proper failure code
        }

        auto response_data = response->GetData();

        const bluetooth::HidReport *data_report;
        if (hos::GetVersion() >= hos::Version_9_0_0) {
            data_report = &response_data.data_report.v9.report;
        } else if (hos::GetVersion() >= hos::Version_7_0_0) {
            data_report = reinterpret_cast<const bluetooth::HidReport *>(&response_data.data_report.v7.report);
        } else {
            data_report = reinterpret_cast<const bluetooth::HidReport *>(&response_data.data_report.v1.report);
        }

        out_report->size = data_report->size;
        std::memcpy(&out_report->data, &data_report->data, data_report->size);

        return ams::ResultSuccess();
    }

    Result SwitchController::SetFeatureReport(const bluetooth::HidReport *report) {
        auto response = std::make_shared<HidResponse>(BtdrvHidEventType_SetReport);
        m_future_responses.push(response);
        ON_SCOPE_EXIT { m_future_responses.pop(); };

        R_TRY(btdrvSetHidReport(m_address, BtdrvBluetoothHhReportType_Feature, report));

        if (!response->TimedWait(ams::TimeSpan::FromMilliSeconds(500))) {
            return -1; // This should return a proper failure code
        }

        auto response_data = response->GetData();

        return response_data.set_report.res;
    }

    Result SwitchController::GetFeatureReport(uint8_t id, bluetooth::HidReport *out_report) {
        auto response = std::make_shared<HidResponse>(BtdrvHidEventType_GetReport);
        m_future_responses.push(response);
        ON_SCOPE_EXIT { m_future_responses.pop(); };

        R_TRY(btdrvGetHidReport(m_address, id, BtdrvBluetoothHhReportType_Feature));

        if (!response->TimedWait(ams::TimeSpan::FromMilliSeconds(500))) {
            return -1; // This should return a proper failure code
        }

        auto response_data = response->GetData();
        
        Result result;
        const bluetooth::HidReport *get_report;
        if (hos::GetVersion() >= hos::Version_9_0_0) {
            result = response_data.get_report.v9.res;
            get_report = &response_data.get_report.v9.report;
        } else {
            result = response_data.get_report.v1.res;
            get_report = reinterpret_cast<const bluetooth::HidReport *>(&response_data.get_report.v1.report);
        }

        if (R_SUCCEEDED(result)) {
            out_report->size = get_report->size;
            std::memcpy(&out_report->data, &get_report->data, get_report->size);
        }

        return result;
    }

    void SwitchController::UpdateControllerState(const bluetooth::HidReport *report) {
        m_input_report.size = report->size;
        std::memcpy(m_input_report.data, report->data, report->size);
    }

    void SwitchController::ApplyButtonCombos(SwitchButtonData *buttons) {
        // Home combo = MINUS + DPAD_DOWN
        if (buttons->minus && buttons->dpad_down) {
            buttons->home = 1;
            buttons->minus = 0;
            buttons->dpad_down = 0;
        }

        // Capture combo = MINUS + DPAD_UP
        if (buttons->minus && buttons->dpad_up) {
            buttons->capture = 1;
            buttons->minus = 0;
            buttons->dpad_up = 0;
        }
    }

}
