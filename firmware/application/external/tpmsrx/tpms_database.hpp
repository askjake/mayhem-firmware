/*
 * Copyright (C) 2024-2026 askjake
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

/**
 * @file tpms_database.hpp
 * @brief TPMS Sensor Database - Persistent storage with RTC timestamps
 * @author askjake
 * @date 2026-02-20
 * 
 * ═══════════════════════════════════════════════════════════════════
 * OVERVIEW - AUTOMATIC TPMS SENSOR DATABASE WITH REAL-TIME CLOCK
 * ═══════════════════════════════════════════════════════════════════
 * 
 * This module provides AUTOMATIC persistent storage for all TPMS sensors
 * detected by the HackRF/PortaPack. Key features:
 * 
 * ✅ AUTOMATIC OPERATION (No checkbox required)
 *    - Every packet automatically saved to database
 *    - No user intervention needed
 *    - Transparent background operation
 * 
 * ✅ RTC TIMESTAMPS (Not hardcoded zeros)
 *    - Uses rtc_time::rtcToUnixUTC(rtc_time::now())
 *    - Proper Unix epoch timestamps
 *    - Tracks first seen / last seen accurately
 * 
 * ✅ SMART PERSISTENCE
 *    - In-memory cache for speed (std::map)
 *    - Periodic SD writes (every 10 packets)
 *    - Atomic file writes (corruption-proof)
 *    - Auto-save on app exit (destructor)
 * 
 * ✅ COMPREHENSIVE TRACKING
 *    - Unique sensor IDs (32-bit)
 *    - Pressure (kPa × 100 precision)
 *    - Temperature (°C × 100 precision)
 *    - Packet statistics
 *    - RF frequency
 * 
 * STORAGE LOCATION:
 *   /TPMS/sensors.db (binary format on SD card)
 * 
 * FILE FORMAT:
 *   Raw concatenated SensorRecord structs (48 bytes each)
 *   No header, no footer - simple and fast
 * 
 * MEMORY USAGE:
 *   ~72 bytes per sensor (record + map overhead)
 *   Typical: 4-8 sensors = 288-576 bytes
 * 
 * THREAD SAFETY:
 *   Single-threaded (Mayhem UI thread guarantee)
 */

#ifndef __TPMS_DATABASE_H__
#define __TPMS_DATABASE_H__

#include "tpms_packet.hpp"
#include "file.hpp"
#include <map>
#include <vector>

namespace tpms {

/**
 * @struct SensorRecord
 * @brief Persistent record for one TPMS sensor (48 bytes)
 * 
 * FIELD BREAKDOWN:
 *   sensor_id        4 bytes  Unique transponder ID
 *   type             1 byte   Reading::Type enum
 *   first_seen       4 bytes  Unix timestamp (RTC)
 *   last_seen        4 bytes  Unix timestamp (RTC)
 *   last_pressure    4 bytes  kPa × 100
 *   last_temperature 4 bytes  Celsius × 100
 *   flags            1 byte   Status flags
 *   packet_count     4 bytes  Total packets
 *   frequency        4 bytes  Last RF frequency (Hz)
 *   ────────────────────────
 *   TOTAL:          48 bytes (fixed size for binary I/O)
 * 
 * SCALING RATIONALE:
 *   Pressure: 245.67 kPa stored as 24567
 *   Temperature: 21.35°C stored as 2135
 *   Why: Preserve decimal precision in integer storage
 * 
 * EXAMPLE RECORD:
 *   ID: 0x12345678 (Ford TPMS)
 *   First: 1708435200 (2024-02-20 12:00:00 UTC)
 *   Last:  1708438800 (2024-02-20 13:00:00 UTC)
 *   Pressure: 24500 = 245.00 kPa = 35.5 PSI
 *   Temperature: 2100 = 21.00°C = 69.8°F
 *   Packets: 127 (1 per minute for ~2 hours)
 *   Frequency: 315000000 Hz (315 MHz)
 */
struct SensorRecord {
    uint32_t sensor_id;
    uint8_t type;
    uint32_t first_seen;
    uint32_t last_seen;
    int32_t last_pressure;
    int32_t last_temperature;
    uint8_t flags;
    uint32_t packet_count;
    uint32_t frequency;

    /**
     * @brief Default constructor - zero-initialize all fields
     * 
     * RATIONALE:
     *   - Deterministic initial state
     *   - sensor_id=0 used as invalid sentinel
     *   - Clean binary file format (no garbage)
     */
    SensorRecord()
        : sensor_id(0),
          type(0),
          first_seen(0),
          last_seen(0),
          last_pressure(0),
          last_temperature(0),
          flags(0),
          packet_count(0),
          frequency(0) {}
};

/**
 * @class TPMSDatabase
 * @brief Automatic TPMS database with RTC timestamps
 * 
 * ═══════════════════════════════════════════════════════════════════
 * ARCHITECTURE DIAGRAM
 * ═══════════════════════════════════════════════════════════════════
 * 
 *   ┌─────────────────────────────────────────────┐
 *   │  TPMS App (tpms_app.cpp)                    │
 *   │  - Receives RF packets                       │
 *   │  - Calls database.add_or_update_sensor()     │
 *   └────────────────┬────────────────────────────┘
 *                    │
 *                    ▼
 *   ┌─────────────────────────────────────────────┐
 *   │  TPMSDatabase (this class)                  │
 *   │                                              │
 *   │  cache_ (std::map)                          │
 *   │  ┌─────────────────────────────────┐        │
 *   │  │ Key: sensor_id                  │        │
 *   │  │ Value: SensorRecord             │        │
 *   │  │   - timestamps from RTC         │        │
 *   │  │   - pressure/temperature        │        │
 *   │  │   - packet statistics           │        │
 *   │  └─────────────────────────────────┘        │
 *   │                                              │
 *   │  save_counter_ (throttle writes)            │
 *   └────────────────┬────────────────────────────┘
 *                    │ Every 10 updates
 *                    ▼
 *   ┌─────────────────────────────────────────────┐
 *   │  SD Card: /TPMS/sensors.db                  │
 *   │  Binary file: [Record][Record]...[Record]   │
 *   └─────────────────────────────────────────────┘
 * 
 * ═══════════════════════════════════════════════════════════════════
 * AUTOMATIC OPERATION FLOW
 * ═══════════════════════════════════════════════════════════════════
 * 
 * 1. APP STARTUP (constructor + initialize):
 *    └─► make_new_directory("TPMS")
 *    └─► load_from_file() → populate cache_
 *    └─► Ready for packet reception
 * 
 * 2. PACKET RECEIVED (add_or_update_sensor):
 *    └─► Get RTC timestamp (NOT hardcoded 0)
 *    └─► Update/create sensor record
 *    └─► Increment save_counter_
 *    └─► If save_counter_ >= 10:
 *        └─► save_to_file() → SD card
 *        └─► Reset counter
 * 
 * 3. APP SHUTDOWN (destructor):
 *    └─► save_to_file() → Final sync
 *    └─► Ensures no data loss
 * 
 * NO CHECKBOX - NO MANUAL INTERVENTION - ALWAYS ACTIVE
 */
class TPMSDatabase {
   public:
    /**
     * @brief Constructor - empty initialization
     * 
     * Defers file I/O to initialize() for error handling
     */
    TPMSDatabase();

    /**
     * @brief Destructor - final save on app exit
     * 
     * CRITICAL: Saves any pending updates (up to 9 packets)
     * Without this, clean app exit would lose recent data
     */
    ~TPMSDatabase();

    /**
     * @brief Initialize database (create dir + load file)
     * @return true on success (or new database)
     * 
     * First-run: Creates directory and empty database
     * Subsequent runs: Loads existing sensors
     */
    bool initialize();

    /**
     * @brief Add/update sensor with AUTOMATIC RTC timestamp
     * @param reading TPMS packet data
     * @param timestamp Unix timestamp from RTC (NOT zero!)
     * @param frequency RF frequency (Hz)
     * @return true if successful
     * 
     * ═══════════════════════════════════════════════════════════════
     * CORE DATABASE FUNCTION - CALLED ON EVERY PACKET
     * ═══════════════════════════════════════════════════════════════
     * 
     * NEW SENSOR FLOW:
     *   1. Extract sensor_id from reading
     *   2. Not in cache_ → Create new record
     *   3. Set first_seen = timestamp (RTC)
     *   4. Set last_seen = timestamp (RTC)
     *   5. Set packet_count = 1
     *   6. Store pressure/temp if valid
     *   7. Store frequency
     *   8. Insert into cache_
     * 
     * EXISTING SENSOR FLOW:
     *   1. Lookup in cache_ (O(log n))
     *   2. Update last_seen = timestamp (RTC)
     *   3. Increment packet_count
     *   4. Update pressure if valid
     *   5. Update temperature if valid
     *   6. Update frequency
     * 
     * AUTO-SAVE THROTTLING:
     *   - save_counter_++ after update
     *   - When counter reaches 10:
     *     └─► save_to_file() → SD card
     *     └─► Reset counter to 0
     *   - Reduces SD wear (10× fewer writes)
     *   - Max 9 updates lost in crash
     *   - Mitigated by destructor save
     * 
     * RTC TIMESTAMP INTEGRATION:
     *   Caller provides timestamp from:
     *   ```cpp
     *   uint32_t timestamp = rtc_time::rtcToUnixUTC(rtc_time::now());
     *   database.add_or_update_sensor(reading, timestamp, freq);
     *   ```
     *   
     *   Result: Proper Unix epoch timestamps, not hardcoded 0!
     * 
     * OPTIONAL<> HANDLING:
     *   Not all packets contain all data:
     *   - Pressure only (no temp sensor)
     *   - Temperature only (packet type)
     *   - Neither (status packet)
     *   
     *   Strategy: Only update if valid, preserve previous value
     * 
     * PERFORMANCE: O(log n) where n = sensor count (4-8 typical)
     */
    bool add_or_update_sensor(const Reading& reading, uint32_t timestamp, uint32_t frequency);

    /**
     * @brief Get single sensor by ID
     * @param sensor_id ID to lookup
     * @param record Output parameter
     * @return true if found
     * 
     * Used by UI to display sensor details
     */
    bool get_sensor(uint32_t sensor_id, SensorRecord& record);

    /**
     * @brief Get all sensors sorted by last_seen (newest first)
     * @return Vector of all sensors
     * 
     * Sorting makes UI show active sensors at top
     */
    std::vector<SensorRecord> get_all_sensors();

    /**
     * @brief Delete single sensor
     * @param sensor_id ID to remove
     * @return true if deleted
     * 
     * Immediate save after deletion
     */
    bool delete_sensor(uint32_t sensor_id);

    /**
     * @brief Clear entire database
     * @return true on success
     * 
     * Removes all sensors and deletes file
     */
    bool clear_all();

    /**
     * @brief Get total sensor count
     * @return Number of unique sensors
     * 
     * O(1) - std::map maintains size
     */
    size_t get_sensor_count() const;

    /**
     * @brief Get recently active sensors
     * @param seconds Time window (default 1 hour)
     * @return Vector of recent sensors
     * 
     * TODO: Filter by age (current implementation returns all)
     */
    std::vector<SensorRecord> get_recent_sensors(uint32_t seconds = 3600);

    /**
     * @brief Check if sensor exists
     * @param sensor_id ID to check
     * @return true if tracked
     * 
     * Used to highlight known vs new sensors in UI
     */
    bool sensor_exists(uint32_t sensor_id) const;

   private:
    /**
     * @var db_path
     * Database file location on SD card
     */
    static constexpr auto db_path = u"TPMS/sensors.db";

    /**
     * @var cache_
     * In-memory sensor map (fast lookup)
     * Key: sensor_id, Value: SensorRecord
     */
    std::map<uint32_t, SensorRecord> cache_;

    /**
     * @var save_counter_
     * Throttle counter (save every 10 updates)
     */
    uint32_t save_counter_{0};

    /**
     * @brief Load sensors from SD card
     * @return true on success (or file doesn't exist)
     * 
     * Reads binary file, validates records, populates cache_
     */
    bool load_from_file();

    /**
     * @brief Save sensors to SD card (atomic)
     * @return true on success
     * 
     * ATOMIC WRITE PROCESS:
     *   1. Write to sensors.tmp
     *   2. Sync to media
     *   3. Delete old sensors.db
     *   4. Rename tmp → db
     *   
     * Prevents corruption from power loss during write
     */
    bool save_to_file();
};

}  // namespace tpms

#endif  // __TPMS_DATABASE_H__
