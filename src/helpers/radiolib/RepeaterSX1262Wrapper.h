#pragma once

#include <helpers/radiolib/CustomSX1262Wrapper.h>

class RepeaterSX1262 : public CustomSX1262 {
  bool _duty_enabled = false, _duty_allowed = true, _duty_failed = false;
  bool _duty_active = false, _rx_activity = false;
  uint32_t _activity_started = 0, _activity_limit = 0;
  int16_t _duty_error = 0;
  uint16_t _sender_preamble = 16;

public:
  RepeaterSX1262(Module* mod) : CustomSX1262(mod) { }

  void setDutyCycleEnabled(bool enabled) {
    if (_duty_enabled != enabled) _duty_failed = false;
    _duty_enabled = enabled;
  }
  void setDutyCycleAllowed(bool allowed) { _duty_allowed = allowed; }
  void setMinimumSenderPreamble(uint16_t symbols) {
    if (_sender_preamble != symbols) _duty_failed = false;
    _sender_preamble = symbols;
  }
  bool isDutyCycleActive() const { return _duty_active; }
  int16_t getDutyCycleError() const { return _duty_error; }
  void disableDutyAfterError() { _duty_failed = true; }
  void paramsChanged(uint32_t limit) {
    _duty_failed = false;
    _activity_limit = limit;
  }

  int16_t standby() override {
    _duty_active = false;
    _rx_activity = false;
    return SX1262::standby();
  }

  int16_t startReceive() override {
    _duty_active = false;
    _rx_activity = false;
    const auto preamble = 1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED;
    const auto flags = RADIOLIB_IRQ_RX_DEFAULT_FLAGS | preamble;
    if (_duty_enabled && _duty_allowed && !_duty_failed) {
      uint32_t rx_us = 0, sleep_us = 0;
      DataRate_t dr = {};
      dr.lora.spreadingFactor = spreadingFactor;
      dr.lora.bandwidth = bandwidthKhz;
      dr.lora.codingRate = codingRate;
      if (_sender_preamble == 0 || _sender_preamble > 32) {
        _duty_error = RADIOLIB_ERR_INVALID_PREAMBLE_LENGTH;
      } else {
        _duty_error = getPacketType() == RADIOLIB_SX126X_PACKET_TYPE_LORA
          ? calculateRxDutyCycle(_sender_preamble, preambleLengthLoRa, 0, &dr, &rx_us, &sleep_us)
          : RADIOLIB_ERR_WRONG_MODEM;
      }
      // Auto can return success while silently choosing continuous RX. Check first.
      if (_duty_error == RADIOLIB_ERR_NONE && sleep_us >= tcxoDelay + 1016) {
        const auto mask = RADIOLIB_IRQ_RX_DEFAULT_MASK | preamble |
          (1UL << RADIOLIB_IRQ_TIMEOUT) | (1UL << RADIOLIB_IRQ_HEADER_ERR);
        _duty_error = startReceiveDutyCycleAuto(_sender_preamble, 0, flags, mask);
        if (_duty_error == RADIOLIB_ERR_NONE) {
          _duty_active = true;
          return RADIOLIB_ERR_NONE;
        }
        _duty_failed = true; // don't retry a failing mode after every packet
        MESH_DEBUG_PRINTLN("RX duty failed: %d; continuous RX", _duty_error);
      } else if (_duty_error == RADIOLIB_ERR_NONE) {
        _duty_error = RADIOLIB_ERR_INVALID_SLEEP_PERIOD;
      }
    }
    return SX1262::startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, flags, RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
  }

  bool receivingDutyPacket() const {
    return _rx_activity || mod->hal->digitalRead(mod->getIrq());
  }

  // Preamble interrupts only record activity. They must not read or restart RX.
  int serviceDutyInterrupt() {
    uint32_t irq = getIrqFlags();
    if (irq & RADIOLIB_SX126X_IRQ_RX_DONE) {
      _rx_activity = false;
      return 1;
    }
    if (irq & (RADIOLIB_SX126X_IRQ_TIMEOUT | RADIOLIB_SX126X_IRQ_HEADER_ERR)) {
      _rx_activity = false;
      return -1;
    }
    if (irq & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED) {
      _rx_activity = true;
      _activity_started = millis();
      clearIrqFlags(RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED);
    }
    return (irq & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED) ? 0 : -1;
  }

  bool dutyActivityTimedOut() const {
    return _rx_activity && millis() - _activity_started > _activity_limit + 1000;
  }

  RadioLibTime_t getTimeOnAir(size_t len) override {
    // Dispatcher queries airtime even while the modem is in its sleep interval.
    // Use RadioLib's calculation without the SPI getPacketType() in its getter.
    DataRate_t dr = {};
    dr.lora.spreadingFactor = spreadingFactor;
    dr.lora.bandwidth = bandwidthKhz;
    dr.lora.codingRate = codingRate < 5 ? codingRate + 4 : (codingRate == 7 ? 8 : codingRate);
    PacketConfig_t pc = {};
    pc.lora.preambleLength = preambleLengthLoRa;
    pc.lora.crcEnabled = crcTypeLoRa;
    pc.lora.implicitHeader = headerType == RADIOLIB_SX126X_LORA_HEADER_IMPLICIT;
    pc.lora.ldrOptimize = ldrOptimize;
    return calculateTimeOnAir(RADIOLIB_MODEM_LORA, dr, pc, len);
  }
};

class RepeaterSX1262Wrapper : public CustomSX1262Wrapper {
  float _last_rssi = 0, _last_snr = 0;
  bool _rx_boosted = SX126X_RX_BOOSTED_GAIN;
  bool _policy_allowed = true, _mode_change = false;
  bool _night_sleeping = false;
  bool _params_pending = false, _tx_power_pending = false;
  uint32_t _recovery_at = 0;
  int16_t _power_error = 0;
  float _freq = LORA_FREQ, _bw = LORA_BW;
  uint8_t _sf = LORA_SF, _cr = 5;
  int8_t _tx_power = LORA_TX_POWER;
  RepeaterSX1262& modem() const { return *(RepeaterSX1262*)_radio; }

  void applyParams() {
    idle();
    CustomSX1262Wrapper::setParams(_freq, _bw, _sf, _cr);
    auto sf = modem().spreadingFactor;
    auto pm = calcMaxPacketMillis(sf, modem().bandwidthKhz, _cr, preambleLengthForSF(sf));
    modem().paramsChanged(pm.preambleMillis + pm.payloadMillis);
    _params_pending = false;
    _mode_change = false;
  }

protected:
  bool isReceivePacketReady() override {
    if (!modem().isDutyCycleActive()) return true;
    int result = modem().serviceDutyInterrupt();
    if (result < 0) {
      idle();
      startRecv();
    }
    return result > 0;
  }
  void onReceiveComplete() override {
    _last_rssi = modem().getRSSI();
    _last_snr = modem().getSNR();
  }
  bool isReceivingPacket() override {
    return modem().isDutyCycleActive() ? modem().receivingDutyPacket() : modem().isReceiving();
  }

public:
  RepeaterSX1262Wrapper(RepeaterSX1262& radio, mesh::MainBoard& board)
    : CustomSX1262Wrapper(radio, board) { }

  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    _freq = freq;
    _bw = bw;
    _sf = sf;
    _cr = cr;
    _params_pending = true;
  }
  void setDutyCycleEnabled(bool enabled) {
    modem().setDutyCycleEnabled(enabled);
    _mode_change = true;
  }
  void setMinimumSenderPreamble(uint16_t symbols) {
    modem().setMinimumSenderPreamble(symbols);
    _mode_change = true;
  }
  bool isDutyCycleActive() const { return isInRecvMode() && modem().isDutyCycleActive(); }
  int16_t getDutyCycleError() const { return modem().getDutyCycleError(); }
  bool dutyCyclePolicyAllowed() const { return _policy_allowed; }
  bool radioBusy() {
    if (_night_sleeping) return false;
    return isTransmitting() || hasRadioInterrupt() || isReceivingPacket();
  }
  bool hasPendingRadioWork() {
    return _params_pending || _tx_power_pending || _mode_change || radioBusy();
  }
  const char* getMode() const {
    if (_night_sleeping) return "night sleep";
    if (isTransmitting()) return "TX";
    if (!isInRecvMode()) return "RX recovery";
    return isDutyCycleActive() ? "RX duty cycle" : "continuous RX";
  }
  void loop() override {
    if (_night_sleeping) return;
    if (_last_rx_error != RADIOLIB_ERR_NONE && !isTransmitting() &&
        uint32_t(millis() - _recovery_at) >= 1000) {
      recoverRadio();
    }
    if (_params_pending && !radioBusy()) applyParams();
    if (_tx_power_pending && !radioBusy()) {
      idle();
      RadioLibWrapper::setTxPower(_tx_power);
      _tx_power_pending = false;
    }
    bool allowed = _threshold == 0 && !_cad_enabled;
    if (allowed != _policy_allowed) {
      _policy_allowed = allowed;
      modem().setDutyCycleAllowed(allowed);
      _mode_change = true;
    }
    if (modem().dutyActivityTimedOut() && !hasRadioInterrupt()) {
      idle();
      startRecv();
    }
    if (_mode_change && !radioBusy()) {
      idle();
      _mode_change = false;
    }
    // SPI polling would wake the SX1262 from the sleep portion of RX duty cycle.
    if (!isDutyCycleActive()) CustomSX1262Wrapper::loop();
  }
  float getLastRSSI() const override { return _last_rssi; }
  float getLastSNR() const override { return _last_snr; }
  float getCurrentRSSI() override {
    return isDutyCycleActive() ? getNoiseFloor() : CustomSX1262Wrapper::getCurrentRSSI();
  }
  bool getRxBoostedGainMode() const override { return _rx_boosted; }
  bool setRxBoostedGainMode(bool enabled) override {
    if (radioBusy()) return false;
    idle();
    if (!CustomSX1262Wrapper::setRxBoostedGainMode(enabled)) return false;
    _rx_boosted = enabled;
    return true;
  }

  void setTxPower(int8_t dbm) {
    _tx_power = dbm;
    _tx_power_pending = true;
  }
  int recvRaw(uint8_t* bytes, int sz) override {
    if (_night_sleeping || _last_rx_error != RADIOLIB_ERR_NONE) return 0;
    return RadioLibWrapper::recvRaw(bytes, sz);
  }
  int16_t getPowerError() const {
    return _last_rx_error != RADIOLIB_ERR_NONE ? _last_rx_error : _power_error;
  }
  bool nightSleeping() const { return _night_sleeping; }

  int16_t tryNightSleep() {
    if (hasPendingRadioWork() || !isInRecvMode() || _last_rx_error != RADIOLIB_ERR_NONE) return 1;
    const uint32_t activity = RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED |
      RADIOLIB_SX126X_IRQ_HEADER_VALID;
    if (modem().getIrqFlags() & activity) {
      // This status read can terminate the sleep portion of RX duty cycle.
      // Leave any detected packet untouched; service it before retrying sleep.
      return 1;
    }
    _power_error = modem().sleep(true); // warm sleep retains all radio configuration
    if (_power_error != RADIOLIB_ERR_NONE) {
      int16_t sleep_error = _power_error;
      wakeNightSleep();
      _power_error = sleep_error;
      return sleep_error;
    }
    markRadioIdle();
    _night_sleeping = true;
    return RADIOLIB_ERR_NONE;
  }

  bool wakeNightSleep() {
    _night_sleeping = false; // keep the application running even if the modem fails
    _power_error = modem().standby();
    markRadioIdle();
    if (_power_error == RADIOLIB_ERR_NONE) {
      startRecv();
      _power_error = _last_rx_error;
    }
    if (_power_error != RADIOLIB_ERR_NONE) return recoverRadio();
    return true;
  }

  bool recoverRadio() {
    _recovery_at = millis();
    modem().disableDutyAfterError();
    // std_init restores board-specific TCXO, regulator and RF switch configuration.
    if (!modem().std_init()) {
      _last_rx_error = RADIOLIB_ERR_CHIP_NOT_FOUND;
      _power_error = _last_rx_error;
      markRadioIdle();
      return false;
    }
    applyParams();
    modem().disableDutyAfterError();
    RadioLibWrapper::setTxPower(_tx_power);
    _tx_power_pending = false;
    CustomSX1262Wrapper::setRxBoostedGainMode(_rx_boosted);
    RadioLibWrapper::begin();
    _mode_change = false;
    startRecv();
    if (_last_rx_error != RADIOLIB_ERR_NONE) _power_error = _last_rx_error;
    MESH_DEBUG_PRINTLN("Radio recovery: %d", _last_rx_error);
    return _last_rx_error == RADIOLIB_ERR_NONE;
  }
};
