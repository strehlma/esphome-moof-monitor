#include "vanmoof_ble.h"
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "mbedtls/aes.h"
#include "sdkconfig.h"

#include <esp_gattc_api.h>

#include <cctype>
#include <cinttypes>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace vanmoof_ble {

static const char *const TAG = "vanmoof_ble";

// Pause after a successful cycle; the bike is read about every 5 minutes.
static const uint32_t READ_INTERVAL_MS = 290000;
// Pause after a failed cycle: 30 s, doubled per consecutive failure up to
// 30 min, so a bike that keeps failing is not woken every 40 s (ha-vanmoof).
static const uint32_t RETRY_PAUSE_MS = 30000;
static const uint32_t MAX_RETRY_PAUSE_MS = 1800000;
// Auth written but nothing plausible decrypted: BT standby (pymoof #7) or a
// wrong key. Short retries do not help there.
static const uint32_t AUTH_SUSPECT_PAUSE_MS = 900000;
// Failed connection attempts (OPEN status != OK) before pausing.
static const uint8_t MAX_OPEN_FAILURES = 3;
// The other bike on this ESP32 is in a cycle: try again after this.
static const uint32_t BUSY_PAUSE_MS = 15000;
// Random spread added to pauses so both bikes drift apart.
static const uint32_t PAUSE_JITTER_MS = 20000;
// Upper bound for one cycle from OPEN to finish. The S3 drops an
// unauthenticated link after about 10 s, so a healthy cycle is far below this.
static const uint32_t CYCLE_WATCHDOG_MS = 30000;
// The S3 reports a placeholder of 100 % right after waking up (ha-vanmoof).
static const uint32_t BATTERY_REREAD_DELAY_MS = 2000;
static const uint8_t MAX_BATTERY_REREADS = 1;
// Disconnect-to-reconnect gap when the GATT cache must be cleared before this
// connection (loop() could not clean in time); the clean needs a closed link.
static const uint32_t CACHE_CLEAN_RECONNECT_MS = 2000;
// Re-enable is deferred by this while the link is not closed yet (max 10x).
static const uint32_t CACHE_CLEAN_WAIT_MS = 1000;
static const uint8_t MAX_CLEAN_WAITS = 10;
// Failed cycles in a row on a cached list (any reason) before clearing it.
static const uint8_t MAX_CACHED_FAILURES = 3;
// Same, once the cache was already cleared in this failure streak (e.g. a
// long BT standby): clear again only rarely, but never block it for good,
// so a list that went stale during a streak is still replaced eventually.
static const uint8_t MAX_CACHED_FAILURES_AFTER_CLEAN = 12;
// Minimum gap between two clean attempts from loop() after a stack error.
static const uint32_t CLEAN_RETRY_MS = 5000;
// Give up a pending clean after this many stack errors in a row.
static const uint8_t MAX_CLEAN_ERRORS = 5;
// Clears triggered by a sign (not by the failure count) at most this often,
// so a recurring error cannot rewrite the NVS cache every cycle.
static const uint32_t SIGN_CLEAN_MIN_GAP_MS = 6UL * 3600UL * 1000UL;

// A re-enabled client counts as "about to connect" for this long; the other
// bike holds back new connections and held reads meanwhile, so a connection
// is not opened only to be closed again at once (each one may wake the bike).
static const uint32_t CONNECT_PENDING_WAIT_MS = 20000;
// Retry delay for a re-enable or held read held back by the other bike.
static const uint32_t DEFER_MS = 5000;
static const uint32_t DEFER_JITTER_MS = 5000;

static const char *const TIMEOUT_REENABLE = "reenable";
static const char *const TIMEOUT_WATCHDOG = "read_watchdog";
static const char *const TIMEOUT_BATTERY_REREAD = "battery_reread";
static const char *const TIMEOUT_NEXT_READ = "next_read";

static bool version_logged = false;

VanMoofBLE *VanMoofBLE::active_cycle_owner_ = nullptr;
std::vector<VanMoofBLE *> VanMoofBLE::instances_;

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void VanMoofBLE::setup() {
  if (!version_logged) {
    ESP_LOGI(TAG, "VanMoof BLE Component v%s", VANMOOF_BLE_VERSION);
    version_logged = true;
  }

  // Parse the hex key once. 32 hex characters = 16 bytes (AES-128).
  this->key_bytes_.clear();
  this->key_valid_ = this->encryption_key_.size() == 32;
  for (size_t i = 0; this->key_valid_ && i < this->encryption_key_.size(); i += 2) {
    char hi = this->encryption_key_[i];
    char lo = this->encryption_key_[i + 1];
    if (!std::isxdigit(static_cast<unsigned char>(hi)) || !std::isxdigit(static_cast<unsigned char>(lo))) {
      this->key_valid_ = false;
      break;
    }
    char buf[3] = {hi, lo, 0};
    this->key_bytes_.push_back(static_cast<uint8_t>(strtoul(buf, nullptr, 16)));
  }
  if (!this->key_valid_) {
    ESP_LOGE(TAG, "Encryption key must be 32 hex characters - bike data cannot be read");
  }

  if (this->version_sensor_ != nullptr) {
    this->version_sensor_->publish_state(VANMOOF_BLE_VERSION);
  }

  instances_.push_back(this);

#ifdef CONFIG_BT_GATTC_CACHE_NVS_FLASH
  // The service list survives reboots in NVS. Clear it once per boot so a
  // restart of the monitor always starts from a fresh service discovery.
  // esp32_ble brings the stack up in its loop(), so loop() below does the
  // clean as soon as the stack is active; the first OPEN is a safety net.
  this->cache_clean_pending_ = true;
#endif
}

void VanMoofBLE::loop() {
  // Pending cache clean (boot or self-healing): run it as soon as the stack
  // is up and this client has no link, ahead of the next connection.
  if (this->cache_clean_pending_ && this->link_closed_() &&
      (this->last_clean_attempt_ms_ == 0 || millis() - this->last_clean_attempt_ms_ >= CLEAN_RETRY_MS)) {
    this->clean_cache_();
  }
}

void VanMoofBLE::update() {
  // Connections are driven by ble_client auto_connect and the pause logic below.
}

void VanMoofBLE::dump_config() {
  ESP_LOGCONFIG(TAG, "VanMoof BLE:");
  ESP_LOGCONFIG(TAG, "  Key valid: %s", YESNO(this->key_valid_));
  ESP_LOGCONFIG(TAG, "  Key ID: %u", this->key_id_);
  ESP_LOGCONFIG(TAG, "  Keep connected: %s", YESNO(this->keep_connected_));
  LOG_SENSOR("  ", "Battery", this->battery_sensor_);
  LOG_SENSOR("  ", "Distance", this->distance_sensor_);
  LOG_TEXT_SENSOR("  ", "Frame Number", this->frame_number_sensor_);
  LOG_BINARY_SENSOR("  ", "Lock State", this->lock_state_sensor_);
  LOG_TEXT_SENSOR("  ", "Version", this->version_sensor_);
  LOG_BINARY_SENSOR("  ", "Charging", this->charging_sensor_);
  LOG_SENSOR("  ", "Module Battery", this->module_battery_sensor_);
  LOG_SENSOR("  ", "Error Code", this->error_code_sensor_);
  LOG_TEXT_SENSOR("  ", "Alarm", this->alarm_sensor_);
  LOG_TEXT_SENSOR("  ", "Alarm Setting", this->alarm_setting_sensor_);
  LOG_TEXT_SENSOR("  ", "Module State", this->module_state_sensor_);
  LOG_TEXT_SENSOR("  ", "Firmware", this->firmware_sensor_);
#ifdef CONFIG_BT_GATTC_CACHE_NVS_FLASH
  ESP_LOGCONFIG(TAG, "  GATT cache: NVS (cleared at boot and when the list looks outdated)");
#else
  ESP_LOGCONFIG(TAG, "  GATT cache: cleared on every disconnect (full discovery each time)");
#endif
}

// ---------------------------------------------------------------------------
// GATT events (only events of this node's own client arrive here)
// ---------------------------------------------------------------------------

void VanMoofBLE::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                     esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      // BLEClientBase treats ALREADY_OPEN as a successful open as well.
      if (param->open.status != ESP_GATT_OK && param->open.status != ESP_GATT_ALREADY_OPEN) {
        // ble_client returns to IDLE and auto_connect retries on the next
        // advertisement; after a few failures in a row, back off instead.
        this->open_failures_++;
        ESP_LOGW(TAG, "Connection attempt failed, status=%d (%u in a row)", param->open.status,
                 this->open_failures_);
        if (this->open_failures_ >= MAX_OPEN_FAILURES) {
          this->open_failures_ = 0;
          if (this->consecutive_failures_ < 255) {
            this->consecutive_failures_++;
          }
          uint32_t pause_ms = this->backoff_pause_ms_();
          ESP_LOGW(TAG, "Too many failed connection attempts - pausing %" PRIu32 " s", pause_ms / 1000);
          this->pause_client_(pause_ms);
        }
        break;
      }
      this->open_failures_ = 0;
      this->connect_pending_ = false;
      this->link_lost_ = false;
      this->link_held_ = false;
      this->cancel_timeout(TIMEOUT_NEXT_READ);
      // The base also forwards OPEN when it ignored it or is already closing
      // the link (want_disconnect_); only a link it took over gets a cycle.
      if (this->parent_->state() != esp32_ble_tracker::ClientState::CONNECTED) {
        ESP_LOGD(TAG, "OPEN for a link the client did not take over - ignored");
        break;
      }
      ESP_LOGI(TAG, "Connected to VanMoof bike");
      this->reset_handles_();

      if (this->phase_ != CyclePhase::IDLE) {
        // Should not happen; a previous cycle never saw its disconnect.
        ESP_LOGW(TAG, "New connection while a cycle was still active - resetting cycle");
        this->end_cycle_();
      }

      if (this->cache_clean_pending_) {
        // Only reached if loop() could not clean before this connection
        // (boot race or a failed clean): this link may run on an outdated
        // list. Close it, clear the cache once the link is closed, reconnect.
        ESP_LOGI(TAG, "GATT cache still to be cleared - reconnecting in %" PRIu32 " s",
                 CACHE_CLEAN_RECONNECT_MS / 1000);
        this->pause_client_(CACHE_CLEAN_RECONNECT_MS);
        break;
      }

      if (this->last_read_time_ != 0) {
        uint32_t since_ms = millis() - this->last_read_time_;
        if (since_ms < READ_INTERVAL_MS) {
          uint32_t remaining_ms = READ_INTERVAL_MS - since_ms;
          ESP_LOGI(TAG, "Too soon since last read (%" PRIu32 " s ago) - pausing %" PRIu32 " s", since_ms / 1000,
                   remaining_ms / 1000);
          this->pause_client_(remaining_ms);
          break;
        }
      }

      if (active_cycle_owner_ != nullptr && active_cycle_owner_ != this) {
        // The other bike is being read on the same radio; discovery would
        // slow both down past the bike's ~10 s login window.
        uint32_t pause_ms = BUSY_PAUSE_MS + this->jitter_ms_(PAUSE_JITTER_MS);
        ESP_LOGI(TAG, "Other bike is being read - pausing %" PRIu32 " s", pause_ms / 1000);
        this->pause_client_(pause_ms);
        break;
      }
      this->start_cycle_();
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      if (this->phase_ != CyclePhase::DISCOVERING) {
        break;
      }
      if (param->search_cmpl.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Service discovery failed, status=%d", param->search_cmpl.status);
        this->note_cache_status_("service discovery failed", param->search_cmpl.status);
        this->fail_cycle_("service discovery failed");
        break;
      }
      ESP_LOGI(TAG, "Service discovery complete after %" PRIu32 " ms (%s)", millis() - this->cycle_start_ms_,
               this->cycle_fresh_cache_ ? "fresh service list" : "from GATT cache");
      this->discover_characteristics_();
      if (this->char_security_challenge_handle_ == 0 || this->char_security_key_index_handle_ == 0) {
        this->note_cache_suspect_("security characteristics missing");
        this->fail_cycle_("security characteristics not found");
        break;
      }
      if (!this->key_valid_) {
        this->fail_cycle_("encryption key invalid");
        break;
      }
      this->request_challenge_();
      break;
    }

    case ESP_GATTC_CFG_MTU_EVT: {
      ESP_LOGD(TAG, "MTU exchange: status %d, mtu %d", param->cfg_mtu.status, param->cfg_mtu.mtu);
      break;
    }

    case ESP_GATTC_READ_CHAR_EVT: {
      uint16_t handle = param->read.handle;

      // Challenge nonce (first step of the handshake)
      if (this->phase_ == CyclePhase::AUTHENTICATING && handle == this->char_security_challenge_handle_) {
        if (param->read.status != ESP_GATT_OK) {
          ESP_LOGW(TAG, "Challenge read failed, status=%d", param->read.status);
          this->note_cache_status_("challenge read error", param->read.status);
          this->fail_cycle_("challenge read failed");
          break;
        }
        this->send_auth_payload_(param->read.value, param->read.value_len);
        break;
      }

      // Data reads: strictly one outstanding read, matched against the queue
      if (this->phase_ != CyclePhase::READING || this->read_index_ >= this->read_queue_.size() ||
          handle != this->read_queue_[this->read_index_]) {
        ESP_LOGD(TAG, "Ignoring read event for handle 0x%04x", handle);
        break;
      }

      if (this->is_extra_handle_(handle)) {
        // Additional values never affect the cycle result or the cache logic.
        if (param->read.status == ESP_GATT_OK) {
          this->handle_extra_value_(handle, param->read.value, param->read.value_len);
        } else {
          ESP_LOGD(TAG, "Read of optional handle 0x%04x failed, status=%d", handle, param->read.status);
        }
        this->read_index_++;
        this->request_next_read_();
        break;
      }

      bool advance = true;
      if (param->read.status == ESP_GATT_OK) {
        advance = this->handle_value_(handle, param->read.value, param->read.value_len);
      } else {
        ESP_LOGW(TAG, "Read failed, handle=0x%04x, status=%d", handle, param->read.status);
        this->note_cache_status_("read error", param->read.status);
      }
      if (this->implausible_value_) {
        // Garbage after decryption: key or session not valid. Stop now so no
        // further value of this cycle is used.
        // On a held link: reconnect with a fresh login after the normal backoff.
        this->fail_cycle_("implausible decrypted value (wrong key or bike in BT standby?)",
                          this->held_cycle_ ? 0 : AUTH_SUSPECT_PAUSE_MS);
        break;
      }

      if (!advance) {
        // Battery placeholder: read the same handle again after a short delay.
        this->set_timeout(TIMEOUT_BATTERY_REREAD, BATTERY_REREAD_DELAY_MS, [this]() {
          if (this->phase_ == CyclePhase::READING) {
            this->request_next_read_();
          }
        });
        break;
      }
      this->read_index_++;
      this->request_next_read_();
      break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (this->phase_ != CyclePhase::AUTHENTICATING || param->write.handle != this->char_security_key_index_handle_) {
        break;
      }
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Auth payload rejected, status=%d", param->write.status);
        this->note_cache_status_("auth write error", param->write.status);
        this->fail_cycle_("auth write failed");
        break;
      }
      // A confirmed write does not prove the key was accepted; the first
      // decrypted value does (see finish_cycle_).
      ESP_LOGI(TAG, "Auth payload written after %" PRIu32 " ms", millis() - this->cycle_start_ms_);
      this->auth_written_ = true;
      this->start_reads_();
      break;
    }

    case ESP_GATTC_DISCONNECT_EVT: {
      // Mark the link as gone at once; the base only reaches IDLE on CLOSE_EVT.
      this->link_lost_ = true;
      bool held_idle = this->link_held_ && this->phase_ == CyclePhase::IDLE;
      this->cancel_timeout(TIMEOUT_NEXT_READ);
      // A pending read/write ends with an error status when the link drops;
      // a lost link says nothing about the service list.
      this->cache_suspect_ = false;
      if (this->phase_ == CyclePhase::READING && this->got_encrypted_value_ && !this->implausible_value_) {
        // Values already decrypted and published: keep them as a (partial) success
        // instead of waking the bike again 30 s later.
        ESP_LOGW(TAG, "Bike disconnected after %u of %u reads (reason 0x%02x) - keeping values read so far",
                 static_cast<unsigned>(this->read_index_), static_cast<unsigned>(this->read_queue_.size()),
                 param->disconnect.reason);
        this->finish_cycle_();
      } else if (this->phase_ != CyclePhase::IDLE) {
        ESP_LOGW(TAG, "Bike disconnected during read cycle (reason 0x%02x, %" PRIu32 " ms after connect)",
                 param->disconnect.reason, millis() - this->cycle_start_ms_);
        this->fail_cycle_("disconnected by bike or link lost");
      } else {
        ESP_LOGI(TAG, "Disconnected from VanMoof bike (reason 0x%02x)", param->disconnect.reason);
      }
      if (held_idle) {
        // The held link dropped between reads (bike asleep or out of range):
        // reconnect no earlier than the next read is due, so the "too soon"
        // check does not cost an extra connection.
        uint32_t wait_ms = RETRY_PAUSE_MS;
        if (this->last_read_time_ != 0) {
          uint32_t since_ms = millis() - this->last_read_time_;
          if (since_ms < READ_INTERVAL_MS && READ_INTERVAL_MS - since_ms > wait_ms) {
            wait_ms = READ_INTERVAL_MS - since_ms;
          }
        }
        ESP_LOGI(TAG, "Held connection closed - next connection in %" PRIu32 " s at the earliest", wait_ms / 1000);
        this->pause_client_(wait_ms);
      }
      this->reset_handles_();
      break;
    }

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Cycle control
// ---------------------------------------------------------------------------

void VanMoofBLE::start_cycle_(bool held) {
  this->phase_ = CyclePhase::DISCOVERING;
  this->held_cycle_ = held;
  this->cycle_start_ms_ = millis();
  this->got_encrypted_value_ = false;
  this->auth_written_ = false;
  this->implausible_value_ = false;
  this->battery_rereads_ = 0;
  this->pending_battery_ = -1;
  this->pending_lock_ = -1;
  this->pending_distance_valid_ = false;
  this->pending_charging_ = -1;
  this->pending_module_battery_ = -1;
  this->pending_error_code_ = -1;
  this->pending_alarm_ = -1;
  this->pending_alarm_setting_ = -1;
  this->pending_module_state_ = -1;
  this->pending_firmware_.clear();
  this->read_queue_.clear();
  this->read_index_ = 0;
  if (!held) {
    // A held link keeps the service list of its first cycle; the cache
    // flags belong to the next new connection.
#ifdef CONFIG_BT_GATTC_CACHE_NVS_FLASH
    this->cycle_fresh_cache_ = this->cache_fresh_;
#else
    this->cycle_fresh_cache_ = true;  // ESPHome clears the cache on every disconnect
#endif
    this->cache_fresh_ = false;
  }
  this->cache_suspect_ = false;
  this->weak_suspect_ = false;
  active_cycle_owner_ = this;
  this->set_timeout(TIMEOUT_WATCHDOG, CYCLE_WATCHDOG_MS, [this]() {
    if (this->phase_ == CyclePhase::READING && this->got_encrypted_value_ && !this->implausible_value_) {
      ESP_LOGW(TAG, "Cycle timed out during reads - keeping values read so far");
      this->finish_cycle_();
    } else {
      this->fail_cycle_("cycle did not complete within 30 s");
    }
  });
}

void VanMoofBLE::end_cycle_() {
  this->phase_ = CyclePhase::IDLE;
  this->held_cycle_ = false;
  this->cancel_timeout(TIMEOUT_WATCHDOG);
  this->cancel_timeout(TIMEOUT_BATTERY_REREAD);
  this->read_queue_.clear();
  this->read_index_ = 0;
  if (active_cycle_owner_ == this) {
    active_cycle_owner_ = nullptr;
  }
}

void VanMoofBLE::finish_cycle_() {
  if (this->phase_ == CyclePhase::IDLE) {
    return;
  }
  if (this->implausible_value_) {
    // Garbage after decryption means the key or session is not valid.
    this->fail_cycle_("implausible decrypted value (wrong key or bike in BT standby?)",
                      this->held_cycle_ ? 0 : AUTH_SUSPECT_PAUSE_MS);
    return;
  }
  if (!this->got_encrypted_value_) {
    // Reads answered but nothing decrypted after an accepted auth write:
    // standby or wrong key; otherwise a normal short retry. With a cached
    // service list it may also be reads of the wrong handles.
    this->weak_suspect_ = true;
    this->fail_cycle_("no value could be decrypted (authentication not accepted?)",
                      (this->auth_written_ && !this->held_cycle_) ? AUTH_SUSPECT_PAUSE_MS : 0);
    return;
  }
  uint32_t duration_ms = millis() - this->cycle_start_ms_;
  const bool held = this->held_cycle_;  // end_cycle_ clears it
  this->publish_pending_();
  if (held) {
    // Same service list as the cycle that opened the link: no cache verdict.
  } else if (this->cycle_fresh_cache_) {
    // Errors on a fresh list too: they are not caused by the cache.
    this->persistent_suspect_ = this->cache_suspect_;
  } else if (!this->cache_suspect_) {
    this->persistent_suspect_ = false;
  } else if (!this->persistent_suspect_ && this->sign_clean_allowed_()) {
    // Values decrypted, but some step failed: refresh the list next time.
    this->cache_clean_pending_ = true;
    this->last_sign_clean_ms_ = millis() == 0 ? 1 : millis();
    ESP_LOGW(TAG, "Read cycle had errors on a cached service list - GATT cache will be cleared");
  }
  this->streak_cleaned_ = false;
  this->end_cycle_();
  this->consecutive_failures_ = 0;
  this->open_failures_ = 0;
  this->cached_failures_ = 0;
  this->last_read_time_ = millis();
  if (this->last_read_time_ == 0) {
    this->last_read_time_ = 1;  // 0 means "never read"
  }
  uint32_t pause_ms = READ_INTERVAL_MS + this->jitter_ms_(PAUSE_JITTER_MS);
  if (this->keep_connected_ && !this->link_lost_ && !this->cache_clean_pending_ && this->parent_ != nullptr &&
      this->parent_->connected()) {
    ESP_LOGI(TAG, "Read cycle complete in %" PRIu32 " ms - keeping the connection, next read in %" PRIu32 " s",
             duration_ms, pause_ms / 1000);
    this->link_held_ = true;
    this->schedule_held_read_(pause_ms);
    return;
  }
  ESP_LOGI(TAG, "Read cycle complete in %" PRIu32 " ms - pausing %" PRIu32 " s", duration_ms, pause_ms / 1000);
  this->pause_client_(pause_ms);
}

void VanMoofBLE::schedule_held_read_(uint32_t delay_ms) {
  this->set_timeout(TIMEOUT_NEXT_READ, delay_ms, [this]() { this->start_held_cycle_(); });
}

void VanMoofBLE::start_held_cycle_() {
  if (this->phase_ != CyclePhase::IDLE || !this->link_held_) {
    return;
  }
  if (this->link_lost_ || this->parent_ == nullptr || !this->parent_->connected()) {
    // The disconnect handling schedules the next connection.
    this->link_held_ = false;
    return;
  }
  if (this->cache_clean_pending_) {
    ESP_LOGI(TAG, "GATT cache to be cleared - closing the held connection");
    this->pause_client_(CACHE_CLEAN_RECONNECT_MS);
    return;
  }
  if (active_cycle_owner_ != nullptr && active_cycle_owner_ != this) {
    this->schedule_held_read_(BUSY_PAUSE_MS + this->jitter_ms_(PAUSE_JITTER_MS));
    return;
  }
  if (this->other_bike_connecting_()) {
    // Reading now would make the other bike's new connection close at once.
    this->schedule_held_read_(DEFER_MS + this->jitter_ms_(DEFER_JITTER_MS));
    return;
  }
  ESP_LOGI(TAG, "Reading over the held connection");
  this->start_cycle_(true);
  this->start_reads_();
}

void VanMoofBLE::fail_cycle_(const char *reason, uint32_t min_pause_ms) {
  if (this->phase_ == CyclePhase::IDLE) {
    return;
  }
  // Cached service list: clear it before the next try if a stale-list sign
  // shows right after a good cycle (a list that just went stale), or after
  // 3 failures in a row on the cache (12 once cleared in this streak, e.g. a
  // long BT standby). With a fresh list the cause lies elsewhere.
  const bool held = this->held_cycle_;  // end_cycle_ clears it
  bool on_cache = !this->cycle_fresh_cache_ && !held;
  // Strong signs (handle/attribute errors) count any time; weak ones
  // (implausible or no decrypted value, as in BT standby) only right after a
  // good cycle. Sign-triggered clears at most every 6 h.
  bool after_success = this->consecutive_failures_ == 0;
  bool weak = this->implausible_value_ || this->weak_suspect_;
  bool stale_cache =
      on_cache && (this->cache_suspect_ || (weak && after_success)) && this->sign_clean_allowed_();
  this->end_cycle_();
  if (this->consecutive_failures_ < 255) {
    this->consecutive_failures_++;
  }
  if (on_cache) {
    if (this->cached_failures_ < 255) {
      this->cached_failures_++;
    }
  } else if (!held) {
    this->cached_failures_ = 0;
  }
  uint8_t max_cached = this->streak_cleaned_ ? MAX_CACHED_FAILURES_AFTER_CLEAN : MAX_CACHED_FAILURES;
  if (stale_cache || this->cached_failures_ >= max_cached) {
    if (stale_cache) {
      this->last_sign_clean_ms_ = millis() == 0 ? 1 : millis();
    }
    // Pauses stay unchanged: implausible values keep the 15-min pause, so a
    // bike in BT standby is not woken more often; other signs retry in 30 s.
    this->cache_clean_pending_ = true;
    this->streak_cleaned_ = true;
    this->cached_failures_ = 0;
    ESP_LOGW(TAG, "Cycle failed on a cached service list - GATT cache will be cleared before the next try");
  }
  uint32_t pause_ms = this->backoff_pause_ms_();
  if (pause_ms < min_pause_ms) {
    pause_ms = min_pause_ms;
  }
  ESP_LOGW(TAG, "Read cycle failed: %s - failure %u in a row, retrying in %" PRIu32 " s", reason,
           this->consecutive_failures_, pause_ms / 1000);
  this->pause_client_(pause_ms);
}

void VanMoofBLE::publish_pending_() {
  if (this->pending_lock_ >= 0 && this->lock_state_sensor_ != nullptr) {
    this->lock_state_sensor_->publish_state(this->pending_lock_ != 0);
  }
  if (this->pending_distance_valid_ && this->distance_sensor_ != nullptr) {
    this->distance_sensor_->publish_state(this->pending_distance_);
  }
  if (this->pending_battery_ >= 0 && this->battery_sensor_ != nullptr) {
    this->battery_sensor_->publish_state(this->pending_battery_);
  }
  static const char *const ALARM_MODES[] = {"Disarmed", "Armed", "Alarm 1", "Alarm 2", "Alarm 3"};
  static const char *const ALARM_SETTINGS[] = {"Off", "Manual", "Automatic"};
  static const char *const MODULE_STATES[] = {"On",      "Off",     "Shipping", "Standby", "Alarm 1",
                                              "Alarm 2", "Alarm 3", "Sleeping", "Tracking"};
  if (this->pending_charging_ >= 0 && this->charging_sensor_ != nullptr) {
    this->charging_sensor_->publish_state(this->pending_charging_ != 0);
  }
  if (this->pending_module_battery_ >= 0 && this->module_battery_sensor_ != nullptr) {
    this->module_battery_sensor_->publish_state(this->pending_module_battery_);
  }
  if (this->pending_error_code_ >= 0 && this->error_code_sensor_ != nullptr) {
    this->error_code_sensor_->publish_state(this->pending_error_code_);
  }
  if (this->pending_alarm_ >= 0 && this->alarm_sensor_ != nullptr) {
    this->alarm_sensor_->publish_state(ALARM_MODES[this->pending_alarm_]);
  }
  if (this->pending_alarm_setting_ >= 0 && this->alarm_setting_sensor_ != nullptr) {
    this->alarm_setting_sensor_->publish_state(ALARM_SETTINGS[this->pending_alarm_setting_]);
  }
  if (this->pending_module_state_ >= 0 && this->module_state_sensor_ != nullptr) {
    this->module_state_sensor_->publish_state(MODULE_STATES[this->pending_module_state_]);
  }
  if (!this->pending_firmware_.empty() && this->firmware_sensor_ != nullptr) {
    if (this->firmware_sensor_->has_state() && this->firmware_sensor_->get_raw_state() != this->pending_firmware_) {
      // A bike firmware update may change the attribute table: refresh the
      // cached service list before the next connection.
      ESP_LOGW(TAG, "Bike firmware changed from %s to %s - GATT cache will be cleared",
               this->firmware_sensor_->get_raw_state().c_str(), this->pending_firmware_.c_str());
#ifdef CONFIG_BT_GATTC_CACHE_NVS_FLASH
      this->cache_clean_pending_ = true;
#endif
    }
    this->firmware_sensor_->publish_state(this->pending_firmware_);
  }
}

uint32_t VanMoofBLE::backoff_pause_ms_() {
  // 30 s, 60, 120, 240, 480, 960, 1800 (cap) plus jitter
  uint8_t steps = this->consecutive_failures_ == 0 ? 0 : this->consecutive_failures_ - 1;
  if (steps > 6) {
    steps = 6;
  }
  uint32_t pause_ms = RETRY_PAUSE_MS << steps;
  if (pause_ms > MAX_RETRY_PAUSE_MS) {
    pause_ms = MAX_RETRY_PAUSE_MS;
  }
  return pause_ms + this->jitter_ms_(PAUSE_JITTER_MS);
}

uint32_t VanMoofBLE::jitter_ms_(uint32_t max_ms) { return max_ms == 0 ? 0 : random_uint32() % max_ms; }

void VanMoofBLE::pause_client_(uint32_t pause_ms) {
  if (this->parent_ == nullptr) {
    return;
  }
  this->link_held_ = false;
  this->connect_pending_ = false;
  this->cancel_timeout(TIMEOUT_NEXT_READ);
  // set_enabled(false) disconnects and keeps auto_connect from reconnecting.
  this->parent_->set_enabled(false);
  // Named timeout: a new pause replaces a pending one instead of stacking.
  this->set_timeout(TIMEOUT_REENABLE, pause_ms, [this]() {
    if ((active_cycle_owner_ != nullptr && active_cycle_owner_ != this) || this->other_bike_connecting_()) {
      // Connecting now would only end in "Other bike is being read" and an
      // immediate disconnect; wait until the radio is free.
      uint32_t defer_ms = DEFER_MS + this->jitter_ms_(DEFER_JITTER_MS);
      ESP_LOGD(TAG, "Other bike is busy - connecting in %" PRIu32 " s", defer_ms / 1000);
      this->pause_client_(defer_ms);
      return;
    }
    if (this->cache_clean_pending_) {
      if (!this->link_closed_() && this->clean_waits_ < MAX_CLEAN_WAITS) {
        // The clean frees the stack's service cache: never with an open link.
        this->clean_waits_++;
        this->pause_client_(CACHE_CLEAN_WAIT_MS);
        return;
      }
      if (!this->clean_cache_()) {
        // Do not block reconnecting; a later suspect cycle schedules it again.
        this->cache_clean_pending_ = false;
        ESP_LOGW(TAG, "GATT cache could not be cleared - connecting with the cached list");
      }
    }
    this->clean_waits_ = 0;
    ESP_LOGI(TAG, "Re-enabling client for next connection cycle");
    this->connect_pending_ = true;
    this->connect_pending_since_ = millis();
    this->parent_->set_enabled(true);
  });
}

bool VanMoofBLE::other_bike_connecting_() {
  for (auto *inst : instances_) {
    if (inst == this || !inst->connect_pending_) {
      continue;
    }
    if (millis() - inst->connect_pending_since_ < CONNECT_PENDING_WAIT_MS) {
      return true;
    }
    // Expired (e.g. bike out of range): drop the flag so a millis() wrap
    // cannot revive it.
    inst->connect_pending_ = false;
  }
  return false;
}

bool VanMoofBLE::clean_cache_() {
#ifdef CONFIG_BT_GATTC_CACHE_NVS_FLASH
  if (!this->link_closed_()) {
    return false;
  }
  this->last_clean_attempt_ms_ = millis();
  if (this->last_clean_attempt_ms_ == 0) {
    this->last_clean_attempt_ms_ = 1;
  }
  esp_err_t err = esp_ble_gattc_cache_clean(this->parent_->get_remote_bda());
  if (err != ESP_OK) {
    if (++this->clean_errors_ >= MAX_CLEAN_ERRORS) {
      this->clean_errors_ = 0;
      this->cache_clean_pending_ = false;
      ESP_LOGW(TAG, "Clearing GATT cache failed: %d - giving up for now", err);
    } else {
      ESP_LOGW(TAG, "Clearing GATT cache failed: %d", err);
    }
    return false;
  }
  this->clean_errors_ = 0;
  ESP_LOGI(TAG, "GATT cache of %012" PRIX64 " cleared - next connection runs a full service discovery",
           this->parent_->get_address());
  // Bikes with an identical attribute table share one NVS namespace, which
  // this clean erased as well: their next discovery is a fresh one too. Only
  // for idle bikes - one in a cycle may save its list again right after.
  for (auto *inst : instances_) {
    if (inst == this || (inst->link_closed_() && active_cycle_owner_ != inst)) {
      inst->cache_fresh_ = true;
    }
  }
#endif
  this->cache_clean_pending_ = false;
  return true;
}

bool VanMoofBLE::link_closed_() {
  if (this->parent_ == nullptr || esp32_ble::global_ble == nullptr || !esp32_ble::global_ble->is_active()) {
    return false;
  }
  auto state = this->parent_->state();
  return state == esp32_ble_tracker::ClientState::INIT || state == esp32_ble_tracker::ClientState::IDLE;
}

void VanMoofBLE::note_cache_status_(const char *what, int status) {
  // Only statuses that point at a wrong handle or attribute. Codes >= 0x80
  // (e.g. 0x85 GATT_ERROR when the link drops) are stack/link errors.
  switch (status) {
    case 0x01:  // INVALID_HANDLE
    case 0x02:  // READ_NOT_PERMIT
    case 0x03:  // WRITE_NOT_PERMIT
    case 0x05:  // INSUF_AUTHENTICATION
    case 0x0A:  // NOT_FOUND
    case 0x0D:  // INVALID_ATTR_LEN
      this->note_cache_suspect_(what);
      break;
    default:
      break;
  }
}

bool VanMoofBLE::sign_clean_allowed_() {
  return this->last_sign_clean_ms_ == 0 || millis() - this->last_sign_clean_ms_ >= SIGN_CLEAN_MIN_GAP_MS;
}

void VanMoofBLE::note_cache_suspect_(const char *what) {
  if (!this->cache_suspect_) {
    ESP_LOGD(TAG, "Possible sign of an outdated service list: %s", what);
  }
  this->cache_suspect_ = true;
}

void VanMoofBLE::reset_handles_() {
  this->char_security_challenge_handle_ = 0;
  this->char_security_key_index_handle_ = 0;
  this->char_frame_number_handle_ = 0;
  this->char_motor_battery_level_handle_ = 0;
  this->char_distance_handle_ = 0;
  this->char_lock_state_handle_ = 0;
  this->char_alarm_state_handle_ = 0;
  this->char_alarm_mode_handle_ = 0;
  this->char_motor_battery_state_handle_ = 0;
  this->char_module_battery_level_handle_ = 0;
  this->char_firmware_bike_handle_ = 0;
  this->char_module_state_handle_ = 0;
  this->char_errors_handle_ = 0;
}

// ---------------------------------------------------------------------------
// Protocol steps
// ---------------------------------------------------------------------------

void VanMoofBLE::discover_characteristics_() {
  esp_gatt_if_t gattc_if = this->parent_->get_gattc_if();
  uint16_t conn_id = this->parent_->get_conn_id();

  auto find_char = [gattc_if, conn_id](const char *uuid_str) -> uint16_t {
    // UUID string -> little-endian 128-bit UUID as used by ESP-IDF
    uint8_t bytes[16];
    int parsed = sscanf(uuid_str,
                        "%02hhx%02hhx%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-"
                        "%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
                        &bytes[15], &bytes[14], &bytes[13], &bytes[12], &bytes[11], &bytes[10], &bytes[9],
                        &bytes[8], &bytes[7], &bytes[6], &bytes[5], &bytes[4], &bytes[3], &bytes[2], &bytes[1],
                        &bytes[0]);
    if (parsed != 16) {
      return 0;
    }
    esp_bt_uuid_t uuid;
    uuid.len = ESP_UUID_LEN_128;
    memcpy(uuid.uuid.uuid128, bytes, 16);

    esp_gattc_char_elem_t result;
    uint16_t count = 1;
    esp_gatt_status_t status =
        esp_ble_gattc_get_char_by_uuid(gattc_if, conn_id, 0x0001, 0xFFFF, uuid, &result, &count);
    if (status == ESP_GATT_OK && count > 0) {
      return result.char_handle;
    }
    return 0;
  };

  this->char_security_challenge_handle_ = find_char(CHAR_SECURITY_CHALLENGE);
  this->char_security_key_index_handle_ = find_char(CHAR_SECURITY_KEY_INDEX);
  this->char_motor_battery_level_handle_ = find_char(CHAR_MOTOR_BATTERY_LEVEL);
  this->char_distance_handle_ = find_char(CHAR_DISTANCE);
  this->char_frame_number_handle_ = find_char(CHAR_FRAME_NUMBER);
  this->char_lock_state_handle_ = find_char(CHAR_LOCK_STATE);
  this->char_alarm_state_handle_ = find_char(CHAR_ALARM_STATE);
  this->char_alarm_mode_handle_ = find_char(CHAR_ALARM_MODE);
  this->char_motor_battery_state_handle_ = find_char(CHAR_MOTOR_BATTERY_STATE);
  this->char_module_battery_level_handle_ = find_char(CHAR_MODULE_BATTERY_LEVEL);
  this->char_firmware_bike_handle_ = find_char(CHAR_FIRMWARE_BIKE);
  this->char_module_state_handle_ = find_char(CHAR_MODULE_STATE);
  this->char_errors_handle_ = find_char(CHAR_ERRORS);

  ESP_LOGI(TAG, "Handles: Challenge=0x%04x KeyIndex=0x%04x Battery=0x%04x Distance=0x%04x Frame=0x%04x Lock=0x%04x",
           this->char_security_challenge_handle_, this->char_security_key_index_handle_,
           this->char_motor_battery_level_handle_, this->char_distance_handle_, this->char_frame_number_handle_,
           this->char_lock_state_handle_);
}

void VanMoofBLE::request_challenge_() {
  this->phase_ = CyclePhase::AUTHENTICATING;
  esp_err_t err = esp_ble_gattc_read_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(),
                                          this->char_security_challenge_handle_, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Challenge read request failed: %d", err);
    this->fail_cycle_("challenge read request failed");
  }
}

void VanMoofBLE::send_auth_payload_(const uint8_t *nonce, uint16_t nonce_len) {
  // pymoof SX3Profile.build_authentication_payload:
  //   AES-ECB(16 bytes, nonce at [0:2], rest 0) + [0, 0, 0, user_key_id]
  //   (VanMoofKit copies the whole nonce into the block; the S3 sends 2 bytes)
  if (nonce_len == 0 || nonce_len > 16) {
    ESP_LOGW(TAG, "Unexpected challenge nonce length: %u bytes", nonce_len);
    this->note_cache_suspect_("unexpected nonce length");
    this->fail_cycle_("unexpected challenge nonce length");
    return;
  }
  ESP_LOGD(TAG, "Challenge nonce: %u bytes", nonce_len);
  uint8_t plain[16] = {0};
  memcpy(plain, nonce, nonce_len);

  uint8_t payload[20] = {0};
  if (!this->encrypt_block_(plain, payload)) {
    this->fail_cycle_("encrypting auth payload failed");
    return;
  }
  payload[16] = 0;
  payload[17] = 0;
  payload[18] = 0;
  payload[19] = this->key_id_;

  esp_err_t err = esp_ble_gattc_write_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(),
                                           this->char_security_key_index_handle_, sizeof(payload), payload,
                                           ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Auth write request failed: %d", err);
    this->fail_cycle_("auth write request failed");
  }
}

void VanMoofBLE::start_reads_() {
  this->phase_ = CyclePhase::READING;
  this->read_queue_.clear();
  this->read_index_ = 0;
  // Battery last: gives the BMS time to report the real value after wake-up.
  // The frame number never changes; read it only until published once.
  if (this->frame_number_sensor_ != nullptr && this->char_frame_number_handle_ != 0 && !this->frame_published_)
    this->read_queue_.push_back(this->char_frame_number_handle_);
  if (this->lock_state_sensor_ != nullptr && this->char_lock_state_handle_ != 0)
    this->read_queue_.push_back(this->char_lock_state_handle_);
  if (this->distance_sensor_ != nullptr && this->char_distance_handle_ != 0)
    this->read_queue_.push_back(this->char_distance_handle_);
  if (this->battery_sensor_ != nullptr && this->char_motor_battery_level_handle_ != 0)
    this->read_queue_.push_back(this->char_motor_battery_level_handle_);
  // Additional values last: a link drop there keeps the values above
  // (partial success). Only for configured sensors and existing handles.
  auto add_extra = [this](void *sensor, uint16_t handle) {
    if (sensor != nullptr && handle != 0)
      this->read_queue_.push_back(handle);
  };
  add_extra(this->charging_sensor_, this->char_motor_battery_state_handle_);
  add_extra(this->module_battery_sensor_, this->char_module_battery_level_handle_);
  add_extra(this->alarm_sensor_, this->char_alarm_mode_handle_);
  add_extra(this->alarm_setting_sensor_, this->char_alarm_state_handle_);
  add_extra(this->module_state_sensor_, this->char_module_state_handle_);
  add_extra(this->error_code_sensor_, this->char_errors_handle_);
  add_extra(this->firmware_sensor_, this->char_firmware_bike_handle_);

  if (this->read_queue_.empty()) {
    this->fail_cycle_("no readable characteristics found");
    return;
  }
  this->request_next_read_();
}

void VanMoofBLE::request_next_read_() {
  while (this->read_index_ < this->read_queue_.size()) {
    if (this->request_read_(this->read_queue_[this->read_index_])) {
      return;  // wait for ESP_GATTC_READ_CHAR_EVT
    }
    this->read_index_++;
  }
  this->finish_cycle_();
}

bool VanMoofBLE::request_read_(uint16_t handle) {
  esp_err_t err = esp_ble_gattc_read_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(), handle,
                                          ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Read request for handle 0x%04x failed: %d", handle, err);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

bool VanMoofBLE::handle_value_(uint16_t handle, const uint8_t *value, uint16_t value_len) {
  // Frame number: plain ASCII, no authentication needed. Does not count as
  // proof of a successful login.
  if (handle == this->char_frame_number_handle_) {
    std::string frame;
    for (uint16_t i = 0; i < value_len; i++) {
      unsigned char c = value[i];
      if (c != 0 && std::isprint(c)) {
        frame.push_back(static_cast<char>(c));
      }
    }
    if (frame.empty()) {
      ESP_LOGW(TAG, "Frame number empty");
    } else {
      ESP_LOGI(TAG, "Frame number: '%s'", frame.c_str());
      this->frame_number_sensor_->publish_state(frame);
      this->frame_published_ = true;
    }
    return true;
  }

  std::vector<uint8_t> plain;
  if (!this->decrypt_(value, value_len, plain)) {
    ESP_LOGW(TAG, "Could not decrypt value of handle 0x%04x (%u bytes)", handle, value_len);
    return true;
  }
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
  {
    char hex[16 * 3 + 1] = {0};
    for (size_t i = 0; i < plain.size() && i < 16; i++) {
      snprintf(hex + i * 3, 4, "%02X ", plain[i]);
    }
    ESP_LOGD(TAG, "Handle 0x%04x decrypted: %s", handle, hex);
  }
#endif

  if (handle == this->char_motor_battery_level_handle_) {
    uint8_t level = plain[0];
    if (level > 100) {
      ESP_LOGW(TAG, "Implausible battery level %u - not published", level);
      this->implausible_value_ = true;
      return true;
    }
    if (level == 100 && this->battery_rereads_ < MAX_BATTERY_REREADS) {
      this->battery_rereads_++;
      ESP_LOGI(TAG, "Battery reports 100%% - re-reading in %" PRIu32 " ms (wake-up placeholder)",
               BATTERY_REREAD_DELAY_MS);
      return false;
    }
    ESP_LOGI(TAG, "Battery level: %u%%", level);
    this->pending_battery_ = level;
    this->got_encrypted_value_ = true;
    return true;
  }

  if (handle == this->char_distance_handle_) {
    // pymoof decodes all 16 bytes as one little-endian integer, so a real
    // value has zeros beyond the first 4 bytes; anything else is garbage.
    for (size_t i = 4; i < plain.size(); i++) {
      if (plain[i] != 0) {
        ESP_LOGW(TAG, "Implausible distance block (non-zero padding) - not published");
        this->implausible_value_ = true;
        return true;
      }
    }
    uint32_t hm = static_cast<uint32_t>(plain[0]) | (static_cast<uint32_t>(plain[1]) << 8) |
                  (static_cast<uint32_t>(plain[2]) << 16) | (static_cast<uint32_t>(plain[3]) << 24);
    float km = hm / 10.0f;  // pymoof: int.from_bytes(little) / 10
    // Odometer: should never go down (tolerance for float rounding). The
    // padding check above already rejects garbage, so a lower value is held
    // back, not treated as a failed login; after 3 cycles in a row it is
    // accepted (odometer reset after a module swap or firmware reset).
    if (this->distance_sensor_->has_state() && km + 0.05f < this->distance_sensor_->get_raw_state()) {
      this->distance_lower_count_++;
      if (this->distance_lower_count_ < 3) {
        ESP_LOGW(TAG, "Distance %.1f km below last value %.1f km (%u in a row) - not published", km,
                 this->distance_sensor_->get_raw_state(), this->distance_lower_count_);
        this->got_encrypted_value_ = true;
        return true;
      }
      ESP_LOGW(TAG, "Distance %.1f km below last value for 3 cycles - accepting it", km);
    }
    this->distance_lower_count_ = 0;
    ESP_LOGI(TAG, "Distance: %.1f km", km);
    this->pending_distance_ = km;
    this->pending_distance_valid_ = true;
    this->got_encrypted_value_ = true;
    return true;
  }

  if (handle == this->char_lock_state_handle_) {
    uint8_t state = plain[0];  // 0 unlocked, 1 locked, 2 awaiting unlock (pymoof LockState)
    if (state > 2) {
      ESP_LOGW(TAG, "Unknown lock state %u - not published", state);
      this->implausible_value_ = true;
      return true;
    }
    ESP_LOGI(TAG, "Lock state: %s", state == 0 ? "UNLOCKED" : (state == 1 ? "LOCKED" : "AWAITING_UNLOCK"));
    this->pending_lock_ = static_cast<int8_t>(state);
    this->got_encrypted_value_ = true;
    return true;
  }

  return true;
}

bool VanMoofBLE::is_extra_handle_(uint16_t handle) const {
  if (handle == 0) {
    return false;
  }
  return handle == this->char_alarm_state_handle_ || handle == this->char_alarm_mode_handle_ ||
         handle == this->char_motor_battery_state_handle_ || handle == this->char_module_battery_level_handle_ ||
         handle == this->char_firmware_bike_handle_ || handle == this->char_module_state_handle_ ||
         handle == this->char_errors_handle_;
}

void VanMoofBLE::handle_extra_value_(uint16_t handle, const uint8_t *value, uint16_t value_len) {
  std::vector<uint8_t> plain;
  if (!this->decrypt_(value, value_len, plain)) {
    ESP_LOGD(TAG, "Could not decrypt optional handle 0x%04x (%u bytes)", handle, value_len);
    return;
  }

  if (handle == this->char_firmware_bike_handle_) {
    // UTF-8 text, zero padded: printable characters up to the first zero byte
    std::string fw;
    for (uint8_t b : plain) {
      if (b == 0)
        break;
      if (!std::isprint(b)) {
        ESP_LOGD(TAG, "Firmware version contains non-printable bytes - not published");
        return;
      }
      fw.push_back(static_cast<char>(b));
    }
    if (fw.empty()) {
      ESP_LOGD(TAG, "Firmware version empty - not published");
      return;
    }
    ESP_LOGD(TAG, "Firmware: %s", fw.c_str());
    this->pending_firmware_ = fw;
    return;
  }

  // One-byte values: byte 0 holds the value, the rest of the block is zero
  // (module state: byte 1 carries an unknown second value and is not checked).
  size_t first_zero = handle == this->char_module_state_handle_ ? 2 : 1;
  for (size_t i = first_zero; i < plain.size(); i++) {
    if (plain[i] != 0) {
      ESP_LOGD(TAG, "Optional handle 0x%04x: non-zero padding - not published", handle);
      return;
    }
  }
  uint8_t v = plain[0];
  auto check = [handle, v](uint8_t max, const char *what) -> bool {
    if (v > max) {
      ESP_LOGW(TAG, "Unexpected %s value %u (handle 0x%04x) - not published", what, v, handle);
      return false;
    }
    ESP_LOGD(TAG, "%s: %u", what, v);
    return true;
  };
  if (handle == this->char_motor_battery_state_handle_) {
    if (check(1, "Charging state"))
      this->pending_charging_ = v;
  } else if (handle == this->char_module_battery_level_handle_) {
    if (check(100, "Module battery level"))
      this->pending_module_battery_ = v;
  } else if (handle == this->char_errors_handle_) {
    ESP_LOGD(TAG, "Error code: %u", v);
    this->pending_error_code_ = v;
  } else if (handle == this->char_alarm_mode_handle_) {
    if (check(4, "Alarm mode"))
      this->pending_alarm_ = v;
  } else if (handle == this->char_alarm_state_handle_) {
    if (check(2, "Alarm setting"))
      this->pending_alarm_setting_ = v;
  } else if (handle == this->char_module_state_handle_) {
    if (check(8, "Module state"))
      this->pending_module_state_ = v;
  }
}

// ---------------------------------------------------------------------------
// Crypto (AES-128-ECB, as pymoof)
// ---------------------------------------------------------------------------

bool VanMoofBLE::decrypt_(const uint8_t *data, uint16_t len, std::vector<uint8_t> &out) {
  if (!this->key_valid_ || len == 0 || len % 16 != 0) {
    return false;
  }
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_dec(&aes, this->key_bytes_.data(), 128) != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }
  out.assign(len, 0);
  for (uint16_t i = 0; i < len; i += 16) {
    if (mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, data + i, out.data() + i) != 0) {
      mbedtls_aes_free(&aes);
      out.clear();
      return false;
    }
  }
  mbedtls_aes_free(&aes);
  return true;
}

bool VanMoofBLE::encrypt_block_(const uint8_t *in, uint8_t *out) {
  if (!this->key_valid_) {
    return false;
  }
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  bool ok = mbedtls_aes_setkey_enc(&aes, this->key_bytes_.data(), 128) == 0 &&
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out) == 0;
  mbedtls_aes_free(&aes);
  return ok;
}

}  // namespace vanmoof_ble
}  // namespace esphome
