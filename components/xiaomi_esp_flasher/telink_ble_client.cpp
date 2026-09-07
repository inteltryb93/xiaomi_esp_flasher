#include "telink_ble_client.h"
#ifdef USE_ESP32
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <esp_gattc_api.h>
#include <esp_gap_ble_api.h>

namespace esphome {
namespace xiaomi_esp_flasher {

static const char *const TAG = "xiaomi_flasher.ble";

void TelinkBleClient::setup() {
  BLEClientBase::setup();
  this->set_auto_connect(false);
  this->set_connection_type(espbt::ConnectionType::V1);
}

void TelinkBleClient::dump_config() { ESP_LOGCONFIG(TAG, "TelinkBleClient (connection slot %u)", this->get_connection_index()); }

void TelinkBleClient::loop() {
  BLEClientBase::loop();
  if (this->op_ != Op::NONE && this->op_timeout_ && (millis() - this->op_started_) > this->op_timeout_) {
    ESP_LOGW(TAG, "GATT operation %d on handle 0x%04X timed out", (int) this->op_, this->op_handle_);
    this->finish_op_(GATT_ERR_TIMEOUT);
  }
}

bool TelinkBleClient::begin_connect(uint64_t address, esp_ble_addr_type_t addr_type) {
  if (this->state() != espbt::ClientState::IDLE) {
    ESP_LOGW(TAG, "begin_connect while state=%s", espbt::client_state_to_string(this->state()));
    return false;
  }
  this->set_address(address);
  this->set_remote_addr_type(addr_type);
  this->connect_reported_ = false;
  this->congested_ = false;
  // DISCOVERED makes the tracker stop scanning and call connect() for us (esp32_ble_tracker try_promote_discovered_clients_)
  this->set_state(espbt::ClientState::DISCOVERED);
  return true;
}

void TelinkBleClient::end_connection() {
  if (this->op_ != Op::NONE)
    this->finish_op_(GATT_ERR_DISCONNECTED);
  if (this->state() == espbt::ClientState::DISCOVERED) {
    this->set_state(espbt::ClientState::IDLE);
    return;
  }
  if (this->state() != espbt::ClientState::IDLE)
    this->disconnect();
}

std::vector<espbt::ESPBTUUID> TelinkBleClient::service_uuids() {
  std::vector<espbt::ESPBTUUID> out;
  if (this->services_released_)
    return out;
  for (auto *svc : this->services_)
    out.push_back(svc->uuid);
  return out;
}

bool TelinkBleClient::has_service(const espbt::ESPBTUUID &uuid) {
  if (this->services_released_)
    return false;
  for (auto *svc : this->services_)
    if (svc->uuid == uuid)
      return true;
  return false;
}

bool TelinkBleClient::find_characteristic(const espbt::ESPBTUUID &service, const espbt::ESPBTUUID &chr, GattChar &out) {
  if (this->services_released_ || !this->is_established())
    return false;
  auto *c = this->get_characteristic(service, chr);
  if (c == nullptr)
    return false;
  out.handle = c->handle;
  out.props = c->properties;
  out.uuid = c->uuid;
  out.service_uuid = service;
  out.cccd_handle = 0;
  c->parse_descriptors();
  auto *d = c->get_descriptor(0x2902);
  if (d != nullptr)
    out.cccd_handle = d->handle;
  return true;
}

void TelinkBleClient::arm_timeout_(uint32_t ms) {
  this->op_started_ = millis();
  this->op_timeout_ = ms;
}

void TelinkBleClient::finish_op_(int status, const uint8_t *data, size_t len) {
  Op op = this->op_;
  this->op_ = Op::NONE;
  this->op_timeout_ = 0;
  if (op == Op::READ) {
    auto cb = std::move(this->read_cb_);
    this->read_cb_ = nullptr;
    if (cb)
      cb(status, data, len);
  } else if (op != Op::NONE) {
    auto cb = std::move(this->status_cb_);
    this->status_cb_ = nullptr;
    if (cb)
      cb(status);
  }
}

bool TelinkBleClient::read(uint16_t handle, ReadCallback cb, uint32_t timeout_ms) {
  if (!this->is_established()) { cb(GATT_ERR_DISCONNECTED, nullptr, 0); return false; }
  if (this->op_ != Op::NONE) { cb(GATT_ERR_BUSY, nullptr, 0); return false; }
  this->op_ = Op::READ;
  this->op_handle_ = handle;
  this->read_cb_ = std::move(cb);
  this->arm_timeout_(timeout_ms);
  esp_err_t err = esp_ble_gattc_read_char(this->gattc_if_, this->conn_id_, handle, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_ble_gattc_read_char failed: %d", err);
    this->finish_op_(ESP_GATT_ERROR);
    return false;
  }
  return true;
}

bool TelinkBleClient::write(uint16_t handle, const uint8_t *data, size_t len, bool with_response, StatusCallback cb,
                            uint32_t timeout_ms) {
  if (!this->is_established()) { cb(GATT_ERR_DISCONNECTED); return false; }
  if (this->op_ != Op::NONE) { cb(GATT_ERR_BUSY); return false; }
  this->op_ = Op::WRITE;
  this->op_handle_ = handle;
  this->status_cb_ = std::move(cb);
  this->arm_timeout_(timeout_ms);
  esp_err_t err = esp_ble_gattc_write_char(this->gattc_if_, this->conn_id_, handle, len, const_cast<uint8_t *>(data),
                                           with_response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
                                           ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_ble_gattc_write_char failed: %d", err);
    this->finish_op_(ESP_GATT_ERROR);
    return false;
  }
  return true;
}

bool TelinkBleClient::subscribe(const GattChar &chr, StatusCallback cb, uint32_t timeout_ms) {
  if (!this->is_established()) { cb(GATT_ERR_DISCONNECTED); return false; }
  if (this->op_ != Op::NONE) { cb(GATT_ERR_BUSY); return false; }
  this->op_ = Op::SUBSCRIBE_REG;
  this->op_handle_ = chr.handle;
  this->pending_cccd_ = chr.cccd_handle;
  this->status_cb_ = std::move(cb);
  this->arm_timeout_(timeout_ms);
  esp_err_t err = this->register_for_notify(chr.handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "register_for_notify failed: %d", err);
    this->finish_op_(ESP_GATT_ERROR);
    return false;
  }
  return true;
}

void TelinkBleClient::request_conn_params(uint16_t min_int, uint16_t max_int, uint16_t latency, uint16_t timeout) {
  if (!this->is_established())
    return;
  this->update_conn_params_(min_int, max_int, latency, timeout, "ota");
}

bool TelinkBleClient::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                          esp_ble_gattc_cb_param_t *param) {
  // Same filter as BLEClientBase: only events for our app / interface
  if (event == ESP_GATTC_REG_EVT && this->app_id != param->reg.app_id)
    return false;
  if (event != ESP_GATTC_REG_EVT && gattc_if != ESP_GATT_IF_NONE && gattc_if != this->gattc_if_)
    return false;

  bool handled = BLEClientBase::gattc_event_handler(event, gattc_if, param);

  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      if (!this->check_addr(param->open.remote_bda))
        break;
      if (param->open.status != ESP_GATT_OK && param->open.status != ESP_GATT_ALREADY_OPEN) {
        ESP_LOGW(TAG, "[%s] open failed, status=%d", this->address_str(), param->open.status);
        if (!this->connect_reported_ && this->connect_cb_) {
          this->connect_reported_ = true;
          this->connect_cb_(false, param->open.status);
        }
      }
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      if (param->search_cmpl.conn_id != this->conn_id_)
        break;
      ESP_LOGD(TAG, "[%s] service discovery complete, %u services, MTU %u", this->address_str(),
               (unsigned) this->services_.size(), this->mtu_);
      if (!this->connect_reported_ && this->connect_cb_) {
        this->connect_reported_ = true;
        this->connect_cb_(true, 0);
      }
      break;
    }
    case ESP_GATTC_READ_CHAR_EVT: {
      if (param->read.conn_id != this->conn_id_)
        break;
      if (this->op_ == Op::READ && param->read.handle == this->op_handle_) {
        if (param->read.status == ESP_GATT_OK)
          this->finish_op_(ESP_GATT_OK, param->read.value, param->read.value_len);
        else
          this->finish_op_(param->read.status);
      }
      break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (param->write.conn_id != this->conn_id_)
        break;
      if (this->op_ == Op::WRITE && param->write.handle == this->op_handle_)
        this->finish_op_(param->write.status);
      break;
    }
    case ESP_GATTC_CONGEST_EVT: {
      if (param->congest.conn_id != this->conn_id_)
        break;
      this->congested_ = param->congest.congested;
      ESP_LOGV(TAG, "congestion: %d", this->congested_);
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (this->op_ == Op::SUBSCRIBE_REG && param->reg_for_notify.handle == this->op_handle_) {
        if (param->reg_for_notify.status != ESP_GATT_OK) {
          this->finish_op_(param->reg_for_notify.status);
        } else if (this->pending_cccd_ == 0) {
          this->finish_op_(ESP_GATT_OK);  // nothing more to wait for
        } else {
          this->op_ = Op::SUBSCRIBE_CCCD;  // BLEClientBase writes the CCCD; wait for WRITE_DESCR_EVT
        }
      }
      break;
    }
    case ESP_GATTC_WRITE_DESCR_EVT: {
      if (param->write.conn_id != this->conn_id_)
        break;
      if (this->op_ == Op::SUBSCRIBE_CCCD)
        this->finish_op_(param->write.status);
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.conn_id != this->conn_id_)
        break;
      if (this->notify_cb_)
        this->notify_cb_(param->notify.handle, param->notify.value, param->notify.value_len);
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
      if (!this->check_addr(param->disconnect.remote_bda))
        break;
      ESP_LOGD(TAG, "[%s] disconnected, reason 0x%02X", this->address_str(), param->disconnect.reason);
      if (this->op_ != Op::NONE)
        this->finish_op_(GATT_ERR_DISCONNECTED);
      break;
    }
    default:
      break;
  }
  return handled;
}

void TelinkBleClient::on_disconnect_complete(esp_err_t reason) {
  if (this->op_ != Op::NONE)
    this->finish_op_(GATT_ERR_DISCONNECTED);
  bool was_reported = this->connect_reported_;
  this->connect_reported_ = true;
  if (this->connect_cb_)
    this->connect_cb_(false, was_reported ? (int) reason : (int) reason);
}

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
