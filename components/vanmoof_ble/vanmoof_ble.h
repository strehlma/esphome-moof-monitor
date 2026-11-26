#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/log.h"

#include <string>
#include <vector>

namespace esphome {
namespace vanmoof_ble {

// Set from COMPONENT_VERSION in __init__.py (single source).
#ifndef VANMOOF_BLE_COMPONENT_VERSION
#error "VANMOOF_BLE_COMPONENT_VERSION not defined (set by __init__.py)"
#endif
static const char *const VANMOOF_BLE_VERSION = VANMOOF_BLE_COMPONENT_VERSION;

// VanMoof S3/X3 GATT UUIDs (from pymoof, see docs/BLE_PROTOCOL.md)
static const char *const CHAR_SECURITY_CHALLENGE = "6acc5501-e631-4069-944d-b8ca7598ad50";
static const char *const CHAR_SECURITY_KEY_INDEX = "6acc5502-e631-4069-944d-b8ca7598ad50";
static const char *const CHAR_LOCK_STATE = "6acc5521-e631-4069-944d-b8ca7598ad50";
static const char *const CHAR_DISTANCE = "6acc5531-e631-4069-944d-b8ca7598ad50";
static const char *const CHAR_MOTOR_BATTERY_LEVEL = "6acc5541-e631-4069-944d-b8ca7598ad50";
static const char *const CHAR_FRAME_NUMBER = "6acc5552-e631-4069-944d-b8ca7598ad50";
// Additional values (1.10.0). UUIDs and formats from VanMoofKit, confirmed on
// two S3 (firmware 1.09.03) on 2026-11-24. Each is one byte at offset 0 of the
// decrypted block, the rest is zero (module state: byte 1 is set as well).
static const char *const CHAR_ALARM_STATE = "6acc5523-e631-4069-944d-b8ca7598ad50";          // 0 off, 1 manual, 2 automatic
static const char *const CHAR_ALARM_MODE = "6acc5524-e631-4069-944d-b8ca7598ad50";           // 0 disarmed, 1 armed, 2-4 alarm stage 1-3
static const char *const CHAR_MOTOR_BATTERY_STATE = "6acc5542-e631-4069-944d-b8ca7598ad50";  // 0 not charging, 1 charging
static const char *const CHAR_MODULE_BATTERY_LEVEL = "6acc5543-e631-4069-944d-b8ca7598ad50"; // percent
static const char *const CHAR_FIRMWARE_BIKE = "6acc554a-e631-4069-944d-b8ca7598ad50";        // text, e.g. "1.09.03"
static const char *const CHAR_MODULE_STATE = "6acc5562-e631-4069-944d-b8ca7598ad50";         // byte 0: 0 on .. 8 tracking
static const char *const CHAR_ERRORS = "6acc5563-e631-4069-944d-b8ca7598ad50";               // one error code, 0 = none

// Read cycle (one per connection; with keep_connected, later cycles reuse the link):
//   OPEN -> (service discovery by ble_client) -> SEARCH_CMPL
//   -> read challenge -> write auth payload -> sequential reads -> finish
// Decoded values are buffered and published only when the cycle succeeds
// (at least one plausible decrypted value, none implausible). A disconnect
// or timeout after plausible values counts as partial success. Failures
// back off exponentially (30 s .. 30 min); an implausible value or an
// accepted login without decryptable data pauses at least 15 min. Only one
// bike per ESP32 runs a cycle at a time.
//
// GATT cache (CONFIG_BT_GATTC_CACHE_NVS_FLASH): the bike's service list is
// kept in NVS so a connection skips the 8-9 s service discovery. The cache of
// both bikes is cleared once at boot, and again whenever a cycle that used
// the cache shows signs of a stale list (missing handle, GATT error status,
// implausible value); the next cycle then runs a fresh discovery. Within a
// failure streak it is cleared after 3, then only after 12 failures on the
// cache, and pauses stay as they are, so a bike in BT standby is not woken
// more often than before.
//
// keep_connected: after a successful cycle the link stays open and the next
// cycles read over it every ~5 minutes without a new login (each new
// connection may wake the bike from deep sleep, which plays a tune). A cycle
// on the held link that fails closes it; the next connection runs a full
// cycle again. The link keeps the default connection parameters: a slower
// interval made held cycles take 11-24 s instead of 1-3 s. A bike holds back
// its connection or held read while the other bike is busy or about to
// connect, so a link is rarely opened only to be closed at once (a client
// still enabled from an earlier attempt can connect at any time).
enum class CyclePhase : uint8_t {
  IDLE = 0,        // no cycle running
  DISCOVERING,     // connected, waiting for service discovery
  AUTHENTICATING,  // challenge read or auth write in flight
  READING,         // sequential characteristic reads in flight
};

class VanMoofBLE : public ble_client::BLEClientNode, public PollingComponent {
 public:
  VanMoofBLE() : PollingComponent(60000) {}

  void setup() override;
  void update() override;
  void dump_config() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  void set_encryption_key(const std::string &key) { encryption_key_ = key; }
  void set_key_id(uint8_t id) { key_id_ = id; }
  void set_keep_connected(bool keep) { keep_connected_ = keep; }
  void set_battery_sensor(sensor::Sensor *sensor) { battery_sensor_ = sensor; }
  void set_distance_sensor(sensor::Sensor *sensor) { distance_sensor_ = sensor; }
  void set_frame_number_sensor(text_sensor::TextSensor *sensor) { frame_number_sensor_ = sensor; }
  void set_lock_state_sensor(binary_sensor::BinarySensor *sensor) { lock_state_sensor_ = sensor; }
  void set_version_sensor(text_sensor::TextSensor *sensor) { version_sensor_ = sensor; }
  void set_charging_sensor(binary_sensor::BinarySensor *sensor) { charging_sensor_ = sensor; }
  void set_module_battery_sensor(sensor::Sensor *sensor) { module_battery_sensor_ = sensor; }
  void set_error_code_sensor(sensor::Sensor *sensor) { error_code_sensor_ = sensor; }
  void set_alarm_sensor(text_sensor::TextSensor *sensor) { alarm_sensor_ = sensor; }
  void set_alarm_setting_sensor(text_sensor::TextSensor *sensor) { alarm_setting_sensor_ = sensor; }
  void set_module_state_sensor(text_sensor::TextSensor *sensor) { module_state_sensor_ = sensor; }
  void set_firmware_sensor(text_sensor::TextSensor *sensor) { firmware_sensor_ = sensor; }

 protected:
  // Configuration
  std::string encryption_key_;
  uint8_t key_id_{0};
  std::vector<uint8_t> key_bytes_;  // parsed once in setup()
  bool key_valid_{false};
  bool keep_connected_{false};

  // Sensors
  sensor::Sensor *battery_sensor_{nullptr};
  sensor::Sensor *distance_sensor_{nullptr};
  text_sensor::TextSensor *frame_number_sensor_{nullptr};
  binary_sensor::BinarySensor *lock_state_sensor_{nullptr};
  text_sensor::TextSensor *version_sensor_{nullptr};
  binary_sensor::BinarySensor *charging_sensor_{nullptr};
  sensor::Sensor *module_battery_sensor_{nullptr};
  sensor::Sensor *error_code_sensor_{nullptr};
  text_sensor::TextSensor *alarm_sensor_{nullptr};
  text_sensor::TextSensor *alarm_setting_sensor_{nullptr};
  text_sensor::TextSensor *module_state_sensor_{nullptr};
  text_sensor::TextSensor *firmware_sensor_{nullptr};

  // GATT handles of the current connection (0 = not found)
  uint16_t char_security_challenge_handle_{0};
  uint16_t char_security_key_index_handle_{0};
  uint16_t char_frame_number_handle_{0};
  uint16_t char_motor_battery_level_handle_{0};
  uint16_t char_distance_handle_{0};
  uint16_t char_lock_state_handle_{0};
  // Additional values: optional, never decide whether a cycle succeeded
  uint16_t char_alarm_state_handle_{0};
  uint16_t char_alarm_mode_handle_{0};
  uint16_t char_motor_battery_state_handle_{0};
  uint16_t char_module_battery_level_handle_{0};
  uint16_t char_firmware_bike_handle_{0};
  uint16_t char_module_state_handle_{0};
  uint16_t char_errors_handle_{0};

  // Cycle state
  CyclePhase phase_{CyclePhase::IDLE};
  uint32_t cycle_start_ms_{0};
  uint32_t last_read_time_{0};  // millis() of the last successful cycle, 0 = never
  bool got_encrypted_value_{false};
  bool auth_written_{false};      // auth payload confirmed by the bike in this cycle
  bool implausible_value_{false};  // a decrypted value failed the plausibility check
  bool frame_published_{false};    // frame number never changes; read it once
  bool held_cycle_{false};         // this cycle reads over a held link (no discovery, no login)
  bool link_lost_{false};          // DISCONNECT seen for the current link (state may lag behind)
  bool link_held_{false};          // link kept open after a successful cycle (keep_connected)
  bool connect_pending_{false};    // client re-enabled, connection not open yet
  uint32_t connect_pending_since_{0};  // millis() of that re-enable
  uint8_t battery_rereads_{0};
  uint8_t consecutive_failures_{0};
  uint8_t distance_lower_count_{0};  // consecutive cycles with an odometer below the last value
  // Values of the running cycle, published in finish_cycle_ only
  int16_t pending_battery_{-1};
  int8_t pending_lock_{-1};
  bool pending_distance_valid_{false};
  float pending_distance_{0.0f};
  int16_t pending_charging_{-1};
  int16_t pending_module_battery_{-1};
  int16_t pending_error_code_{-1};
  int16_t pending_alarm_{-1};
  int16_t pending_alarm_setting_{-1};
  int16_t pending_module_state_{-1};
  std::string pending_firmware_;  // empty = not read in this cycle
  uint8_t open_failures_{0};
  // GATT cache handling (only relevant with CONFIG_BT_GATTC_CACHE_NVS_FLASH)
  bool cache_clean_pending_{false};  // clear the cache before the next connection
  bool cache_fresh_{false};          // the cache was cleared since the last cycle started
  bool cycle_fresh_cache_{false};    // this cycle runs on a freshly discovered service list
  bool cache_suspect_{false};        // strong sign of a stale list (handle/attribute error)
  bool weak_suspect_{false};         // nothing decrypted: stale list or BT standby
  uint8_t cached_failures_{0};       // failed cycles in a row on a cached service list
  uint8_t clean_waits_{0};           // re-enable deferrals while waiting for a closed link
  bool streak_cleaned_{false};       // cache cleared in the current failure streak
  bool persistent_suspect_{false};   // suspect signs also occur on a fresh list: not a cache issue
  uint32_t last_clean_attempt_ms_{0};
  uint32_t last_sign_clean_ms_{0};   // last clean triggered by a sign (0 = never)
  uint8_t clean_errors_{0};          // failed clean calls in a row
  // All instances: bikes with an identical attribute table share one NVS
  // namespace, so clearing one bike's cache clears the other's as well.
  static std::vector<VanMoofBLE *> instances_;
  // Only one bike per ESP32 runs a cycle at a time (shared radio)
  static VanMoofBLE *active_cycle_owner_;
  std::vector<uint16_t> read_queue_;
  size_t read_index_{0};

  // Cycle control
  void start_cycle_(bool held = false);
  void start_held_cycle_();
  void schedule_held_read_(uint32_t delay_ms);
  bool other_bike_connecting_();
  void finish_cycle_();
  void fail_cycle_(const char *reason, uint32_t min_pause_ms = 0);
  uint32_t backoff_pause_ms_();
  uint32_t jitter_ms_(uint32_t max_ms);
  void end_cycle_();
  void publish_pending_();
  void pause_client_(uint32_t pause_ms);
  void reset_handles_();
  bool clean_cache_();
  bool link_closed_();
  void note_cache_suspect_(const char *what);
  void note_cache_status_(const char *what, int status);
  bool sign_clean_allowed_();

  // Protocol steps
  void discover_characteristics_();
  void request_challenge_();
  void send_auth_payload_(const uint8_t *nonce, uint16_t nonce_len);
  void start_reads_();
  void request_next_read_();
  bool request_read_(uint16_t handle);

  // Value handling; returns false if the read must be repeated (battery placeholder)
  bool handle_value_(uint16_t handle, const uint8_t *value, uint16_t value_len);
  bool is_extra_handle_(uint16_t handle) const;
  void handle_extra_value_(uint16_t handle, const uint8_t *value, uint16_t value_len);

  // Crypto
  bool decrypt_(const uint8_t *data, uint16_t len, std::vector<uint8_t> &out);
  bool encrypt_block_(const uint8_t *in, uint8_t *out);
};

}  // namespace vanmoof_ble
}  // namespace esphome
