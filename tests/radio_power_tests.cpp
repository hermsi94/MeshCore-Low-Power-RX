#include "../src/helpers/radiolib/RepeaterSX1262Wrapper.h"
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

class RadioHarness : public RepeaterSX1262Wrapper {
public:
  RadioHarness(RepeaterSX1262& radio, mesh::MainBoard& board) : RepeaterSX1262Wrapper(radio, board) { }
  void receive() { startRecv(); }
  bool service() { return isReceivePacketReady(); }
  void tx(bool active) { _state = active ? 3 : 1; }
  void packetPending(bool active) { _interrupt = active; }
  void cad(bool active) { _cad_enabled = active; }
};

extern "C" int test_radio_duty_fallback_and_retry() {
  Module m;
  RepeaterSX1262 radio(&m);
  CHECK(radio.startReceive() == 0 && m.rx_calls == 1 && m.duty_calls == 0);
  radio.setDutyCycleEnabled(true);
  CHECK(radio.startReceive() == 0 && m.duty_calls == 0); // conservative 16-symbol default
  radio.setMinimumSenderPreamble(32);
  CHECK(radio.startReceive() == 0 && radio.isDutyCycleActive());
  m.duty_error = -99;
  CHECK(radio.startReceive() == 0 && !radio.isDutyCycleActive());
  CHECK(radio.getDutyCycleError() == -99);
  auto calls = m.duty_calls;
  CHECK(radio.startReceive() == 0 && m.duty_calls == calls);
  radio.setDutyCycleEnabled(false);
  radio.setDutyCycleEnabled(true);
  m.duty_error = 0;
  CHECK(radio.startReceive() == 0 && radio.isDutyCycleActive());
  return 0;
}

extern "C" int test_radio_unsupported_modulation_and_period() {
  Module m;
  RepeaterSX1262 radio(&m);
  radio.setDutyCycleEnabled(true);
  radio.setMinimumSenderPreamble(32);
  radio.modem_type = 0;
  CHECK(radio.startReceive() == 0 && !radio.isDutyCycleActive() && m.duty_calls == 0);
  radio.modem_type = 1;
  m.sleep_us = 0;
  CHECK(radio.startReceive() == 0 && !radio.isDutyCycleActive() && m.duty_calls == 0);
  CHECK(radio.getDutyCycleError() == RADIOLIB_ERR_INVALID_SLEEP_PERIOD);
  radio.setMinimumSenderPreamble(0);
  CHECK(radio.startReceive() == 0 && m.duty_calls == 0);
  CHECK(radio.getDutyCycleError() == RADIOLIB_ERR_INVALID_PREAMBLE_LENGTH);
  return 0;
}

extern "C" int test_radio_preamble_does_not_restart_packet() {
  Module m;
  mesh::MainBoard board;
  RepeaterSX1262 radio(&m);
  RadioHarness driver(radio, board);
  driver.setDutyCycleEnabled(true);
  driver.setMinimumSenderPreamble(32);
  driver.loop();
  driver.receive();
  auto calls = m.duty_calls;
  m.irq = RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED;
  m.pin = true;
  CHECK(!driver.service());
  CHECK(m.duty_calls == calls && driver.radioBusy());
  CHECK(driver.tryNightSleep() == 1 && m.sleep_calls == 0);
  m.irq = RADIOLIB_SX126X_IRQ_RX_DONE;
  CHECK(driver.service());
  CHECK(m.duty_calls == calls);
  return 0;
}

extern "C" int test_radio_idle_duty_has_no_spi_polling() {
  Module m;
  mesh::MainBoard board;
  RepeaterSX1262 radio(&m);
  RadioHarness driver(radio, board);
  driver.setDutyCycleEnabled(true);
  driver.setMinimumSenderPreamble(32);
  driver.loop();
  driver.receive();
  auto reads = m.spi_reads;
  driver.loop();
  CHECK(!driver.radioBusy());
  driver.getLastRSSI(); driver.getLastSNR(); driver.getCurrentRSSI();
  driver.getRxBoostedGainMode(); radio.getTimeOnAir(200);
  CHECK(m.spi_reads == reads);
  driver.cad(true);
  driver.loop();
  driver.receive();
  CHECK(!driver.isDutyCycleActive());
  return 0;
}

extern "C" int test_radio_night_requires_idle_and_uses_warm_sleep() {
  Module m;
  mesh::MainBoard board;
  RepeaterSX1262 radio(&m);
  RadioHarness driver(radio, board);
  driver.receive();
  driver.tx(true);
  CHECK(!driver.setRxBoostedGainMode(false));
  CHECK(driver.tryNightSleep() == 1 && m.sleep_calls == 0);
  driver.tx(false);
  driver.packetPending(true);
  CHECK(driver.tryNightSleep() == 1 && m.sleep_calls == 0);
  driver.packetPending(false);
  driver.setTxPower(10);
  CHECK(driver.tryNightSleep() == 1 && m.sleep_calls == 0);
  driver.loop(); driver.receive();
  CHECK(driver.tryNightSleep() == 0 && m.retain && driver.nightSleeping());
  CHECK(driver.wakeNightSleep() && !driver.nightSleeping() && driver.isInRecvMode());
  return 0;
}

extern "C" int test_radio_failed_wake_reinitializes_and_keeps_loop_online() {
  Module m;
  mesh::MainBoard board;
  RepeaterSX1262 radio(&m);
  RadioHarness driver(radio, board);
  driver.receive();
  CHECK(driver.tryNightSleep() == 0);
  m.wake_error = -77;
  CHECK(driver.wakeNightSleep() && m.init_calls == 1 && !driver.nightSleeping());
  CHECK(driver.isInRecvMode() && !driver.isDutyCycleActive());
  CHECK(driver.tryNightSleep() == 0);
  m.init_ok = false;
  CHECK(!driver.wakeNightSleep() && !driver.nightSleeping());
  CHECK(driver.getPowerError() != 0);
  m.init_ok = true;
  mock_millis = 1000;
  driver.loop();
  CHECK(driver.isInRecvMode());
  return 0;
}

extern "C" int test_radio_continuous_error_retries_without_busy_loop() {
  Module m;
  mesh::MainBoard board;
  RepeaterSX1262 radio(&m);
  RadioHarness driver(radio, board);
  m.rx_error = -88;
  driver.receive();
  auto calls = m.rx_calls;
  uint8_t bytes[10];
  CHECK(driver.recvRaw(bytes, 10) == 0 && m.rx_calls == calls);
  CHECK(driver.tryNightSleep() == 1 && !driver.nightSleeping());
  driver.loop();
  CHECK(m.init_calls == 0);
  mock_millis = 1000;
  m.rx_error = 0;
  driver.loop();
  CHECK(m.init_calls == 1 && driver.isInRecvMode());
  return 0;
}
