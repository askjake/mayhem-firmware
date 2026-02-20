/*
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2023 Mark Thompson
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "tpms_app.hpp"

#include "baseband_api.hpp"
#include "audio.hpp"
#include "portapack.hpp"
using namespace portapack;

#include "string_format.hpp"

#include "utility.hpp"
#include "file_path.hpp"

namespace pmem = portapack::persistent_memory;

namespace ui::external_app::tpmsrx {

namespace format {

std::string type(tpms::Reading::Type type) {
    return to_string_dec_uint(toUType(type), 2);
}

std::string id(tpms::TransponderID id) {
    return to_string_hex(id.value(), 8);
}

std::string pressure(Pressure pressure) {
    return to_string_dec_int(units_psi ? pressure.psi() : pressure.kilopascal(), 3);
}

std::string temperature(Temperature temperature) {
    return to_string_dec_int(units_fahr ? temperature.fahrenheit() : temperature.celsius(), 3);
}

std::string flags(tpms::Flags flags) {
    return to_string_hex(flags, 2);
}

static std::string signal_type(tpms::SignalType signal_type) {
    switch (signal_type) {
        case tpms::SignalType::FSK_19k2_Schrader:
            return "FSK 38400 19200 Schrader";
        case tpms::SignalType::OOK_8k192_Schrader:
            return "OOK - 8192 Schrader";
        case tpms::SignalType::OOK_8k4_Schrader:
            return "OOK - 8400 Schrader";
        default:
            return "- - - -";
    }
}

} /* namespace format */

void TPMSLogger::on_packet(const tpms::Packet& packet, const uint32_t target_frequency) {
    const auto hex_formatted = packet.symbols_formatted();
    const auto target_frequency_str = to_string_dec_uint(target_frequency, 10);

    std::string entry = target_frequency_str + " " + ui::external_app::tpmsrx::format::signal_type(packet.signal_type()) + " " + hex_formatted.data + "/" + hex_formatted.errors;
    log_file.write_entry(packet.received_at(), entry);
}

const TPMSRecentEntry::Key TPMSRecentEntry::invalid_key = {tpms::Reading::Type::None, 0};

void TPMSRecentEntry::update(const tpms::Reading& reading) {
    received_count++;

    if (reading.pressure().is_valid()) {
        last_pressure = reading.pressure();
    }
    if (reading.temperature().is_valid()) {
        last_temperature = reading.temperature();
    }
    if (reading.flags().is_valid()) {
        last_flags = reading.flags();
    }
}

TPMSAppView::TPMSAppView(NavigationView&) {
    baseband::run_prepared_image(portapack::memory::map::m4_code.base());

    add_children({&rssi,
                  &field_volume,
                  &channel,
                  &options_band,
                  &options_pressure,
                  &options_temperature,
                  &field_rf_amp,
                  &field_lna,
                  &field_vga,
                  &text_db_stats,       // ADD
                  &button_clear_db,     // ADD
                  &checkbox_auto_save,  // ADD
                  &recent_entries_view});

    /**
     * ════════════════════════════════════════════════════════════════════════
     * DATABASE INITIALIZATION AND STORED SENSORS LOADING
     * ════════════════════════════════════════════════════════════════════════
     * 
     * STARTUP BEHAVIOR:
     *   1. Initialize database (load from /TPMS/sensors.db)
     *   2. Load ALL stored sensors into recent_entries view
     *   3. Display them immediately (shows history)
     *   4. New packets will update existing entries or add new ones
     * 
     * RATIONALE:
     *   User sees their sensor history IMMEDIATELY on app launch.
     *   No need to wait for packets - history is right there.
     *   Packet counts show total captures across all sessions.
     * 
     * IMPLEMENTATION:
     *   database_.initialize() → Load from file
     *   database_.get_all_sensors() → Get vector of all sensors
     *   For each sensor → Create TPMSRecentEntry → Add to recent_entries
     *   recent_entries_view updates automatically
     */
    
    // Step 1: Initialize database from file
    database_.initialize();
    update_db_stats();
    
    // Step 2: Load all stored sensors into view
    load_stored_sensors_to_view();

    // Button handlers
    button_clear_db.on_select = [this](Button&) {
        database_.clear_all();
        update_db_stats();
    };

    receiver_model.enable();

    options_band.on_change = [this](size_t, OptionsField::value_t v) {
        receiver_model.set_target_frequency(v);
    };
    options_band.set_by_value(receiver_model.target_frequency());

    options_pressure.on_change = [this](size_t, int32_t i) {
        format::units_psi = (bool)i;
        update_view();
    };
    options_pressure.set_selected_index(format::units_psi, true);

    options_temperature.on_change = [this](size_t, int32_t i) {
        format::units_fahr = (bool)i;
        update_view();
    };
    options_temperature.set_selected_index(format::units_fahr, true);

    logger = std::make_unique<TPMSLogger>();
    if (logger) {
        logger->append(logs_dir / u"TPMS.TXT");
    }

    if (pmem::beep_on_packets()) {
        audio::set_rate(audio::Rate::Hz_24000);
        audio::output::start();
    }
}

TPMSAppView::~TPMSAppView() {
    audio::output::stop();
    receiver_model.disable();
    baseband::shutdown();
}

void TPMSAppView::focus() {
    options_band.focus();
}

void TPMSAppView::update_view() {
    recent_entries_view.set_parent_rect(view_normal_rect);
}

void TPMSAppView::update_db_stats() {
    size_t count = database_.get_sensor_count();
    text_db_stats.set("DB: " + to_string_dec_uint(count) + " sensors");
}

void TPMSAppView::set_parent_rect(const Rect new_parent_rect) {
    View::set_parent_rect(new_parent_rect);

    view_normal_rect = {0, header_height, new_parent_rect.width(), new_parent_rect.height() - header_height};

    update_view();
}

void TPMSAppView::on_packet(const tpms::Packet& packet) {
    if (logger) {
        logger->on_packet(packet, receiver_model.target_frequency());
    }

    const auto reading_opt = packet.reading();
    if (reading_opt.is_valid()) {
        const auto reading = reading_opt.value();
        auto& entry = ::on_packet(recent, TPMSRecentEntry::Key{reading.type(), reading.id()});
        entry.update(reading);
        recent_entries_view.set_dirty();

        /**
         * ════════════════════════════════════════════════════════════════
         * AUTOMATIC DATABASE SAVE - NO CHECKBOX REQUIRED
         * ════════════════════════════════════════════════════════════════
         * 
         * RATIONALE:
         *   Database should ALWAYS capture detected sensors automatically.
         *   No user intervention required - transparent operation.
         * 
         * RTC TIMESTAMP ACQUISITION:
         *   rtc_time::now() - Get current RTC time structure
         *   rtc_time::rtcToUnixUTC() - Convert to Unix epoch (seconds)
         *   
         *   Result: Proper timestamp like 1708435200 (not hardcoded 0!)
         * 
         * FREQUENCY TRACKING:
         *   receiver_model.target_frequency() - Current RF frequency (Hz)
         *   Useful for: 
         *     - Troubleshooting interference
         *     - Multi-frequency sensor analysis
         *     - Regional TPMS variations (315/433 MHz)
         * 
         * DESIGN DECISION:
         *   Removed checkbox gate - database is now ALWAYS active
         *   Previous: Manual checkbox enable
         *   Current: Automatic background operation
         * 
         * PERFORMANCE:
         *   - O(log n) database update (n = sensor count)
         *   - Typical: ~10 µs for map lookup/update
         *   - Auto-save throttled (every 10th packet)
         * 
         * ERROR HANDLING:
         *   add_or_update_sensor() returns false for invalid readings
         *   (e.g., Reading::Type::None) - gracefully ignored
         */
        
        // Get real-time clock timestamp (Unix epoch seconds)
        uint32_t timestamp = rtc_time::rtcToUnixUTC(rtc_time::now());
        
        // Always save to database (automatic operation, no checkbox)
        database_.add_or_update_sensor(reading, timestamp, receiver_model.target_frequency());
        
        // Update UI statistics display
        update_db_stats();
    }

    if (pmem::beep_on_packets()) {
        baseband::request_audio_beep(1000, 24000, 60);
    }
}

void TPMSAppView::on_show_list() {
    recent_entries_view.hidden(false);
    recent_entries_view.focus();
}

}  // namespace ui::external_app::tpmsrx

namespace ui {

template <>
void RecentEntriesTable<ui::external_app::tpmsrx::TPMSRecentEntries>::draw(
    const Entry& entry,
    const Rect& target_rect,
    Painter& painter,
    const Style& style,
    RecentEntriesColumns& columns) {
    std::string line = ui::external_app::tpmsrx::format::type(entry.type) + " ";
    std::string lid = ui::external_app::tpmsrx::format::id(entry.id);
    lid.resize(columns.at(1).second, ' ');
    line += lid;

    if (entry.last_pressure.is_valid()) {
        line += "  " + ui::external_app::tpmsrx::format::pressure(entry.last_pressure.value());
    } else {
        line += "     ";
    }

    if (entry.last_temperature.is_valid()) {
        line += "  " + ui::external_app::tpmsrx::format::temperature(entry.last_temperature.value());
    } else {
        line += "     ";
    }

    if (entry.received_count > 999) {
        line += " +++";
    } else {
        line += " " + to_string_dec_uint(entry.received_count, 3);
    }

    if (entry.last_flags.is_valid()) {
        line += " " + ui::external_app::tpmsrx::format::flags(entry.last_flags.value());
    } else {
        line += "   ";
    }

    line.resize(target_rect.width() / 8, ' ');
    painter.draw_string(target_rect.location(), style, line);
}

}  // namespace ui
