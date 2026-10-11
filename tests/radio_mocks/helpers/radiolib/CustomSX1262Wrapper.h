#pragma once
#include <stdint.h>
#include <stddef.h>

// API fault-injection doubles. These do not simulate SX1262 RF behaviour or SPI.
#define LORA_FREQ 869.618f
#define LORA_BW 62.5f
#define LORA_SF 8
#define LORA_TX_POWER 22
#define SX126X_RX_BOOSTED_GAIN 1
#define MESH_DEBUG_PRINTLN(...)
#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ERR_WRONG_MODEM -20
#define RADIOLIB_ERR_INVALID_SLEEP_PERIOD -21
#define RADIOLIB_ERR_CHIP_NOT_FOUND -22
#define RADIOLIB_ERR_INVALID_PREAMBLE_LENGTH -23
#define RADIOLIB_IRQ_PREAMBLE_DETECTED 2
#define RADIOLIB_IRQ_HEADER_ERR 5
#define RADIOLIB_IRQ_TIMEOUT 9
#define RADIOLIB_IRQ_RX_DEFAULT_FLAGS 0x372UL
#define RADIOLIB_IRQ_RX_DEFAULT_MASK 2UL
#define RADIOLIB_SX126X_RX_TIMEOUT_INF 0xFFFFFFUL
#define RADIOLIB_SX126X_PACKET_TYPE_LORA 1
#define RADIOLIB_SX126X_LORA_HEADER_IMPLICIT 1
#define RADIOLIB_SX126X_IRQ_RX_DONE 2UL
#define RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED 4UL
#define RADIOLIB_SX126X_IRQ_HEADER_VALID 16UL
#define RADIOLIB_SX126X_IRQ_HEADER_ERR 32UL
#define RADIOLIB_SX126X_IRQ_TIMEOUT 512UL
#define RADIOLIB_MODEM_LORA 1
static uint32_t mock_millis;
static uint32_t millis() { return mock_millis; }
using RadioLibTime_t = uint32_t;
struct DataRate_t { struct { uint8_t spreadingFactor; float bandwidth; uint8_t codingRate; } lora; };
struct PacketConfig_t { struct { uint16_t preambleLength; bool crcEnabled, implicitHeader, ldrOptimize; } lora; };
struct Module {
  bool pin = false, init_ok = true, retain = false;
  uint32_t irq = 0, sleep_us = 65536;
  int16_t duty_error = 0, rx_error = 0, sleep_error = 0, wake_error = 0;
  unsigned duty_calls = 0, rx_calls = 0, sleep_calls = 0, init_calls = 0, spi_reads = 0;
  Module* hal = this;
  uint32_t getIrq() const { return 1; }
  bool digitalRead(uint32_t) const { return pin; }
};
class SX1262 {
public:
  Module* mod;
  uint8_t spreadingFactor = 8, codingRate = 1, headerType = 0, crcTypeLoRa = 1, ldrOptimize = 0;
  uint16_t preambleLengthLoRa = 32;
  uint32_t tcxoDelay = 5000;
  float bandwidthKhz = 62.5f, freqMHz = 869.618f;
  int modem_type = 1;
  SX1262(Module* m) : mod(m) { }
  virtual int16_t standby() { return mod->wake_error; }
  virtual int16_t startReceive() { return startReceive(0, 0, 0, 0); }
  int16_t startReceive(uint32_t, uint32_t, uint32_t, size_t = 0) {
    mod->rx_calls++;
    return mod->rx_error;
  }
  uint8_t getPacketType() { mod->spi_reads++; return modem_type; }
  int16_t calculateRxDutyCycle(uint16_t sender, uint16_t, uint16_t, DataRate_t*, uint32_t* rx, uint32_t* sleep) {
    *rx = 36000; *sleep = sender <= 16 ? 0 : mod->sleep_us; return 0;
  }
  int16_t startReceiveDutyCycleAuto(uint16_t, uint16_t, uint32_t, uint32_t) {
    mod->duty_calls++; return mod->duty_error;
  }
  uint32_t getIrqFlags() { mod->spi_reads++; return mod->irq; }
  void clearIrqFlags(uint32_t flags) { mod->irq &= ~flags; mod->pin = mod->irq != 0; }
  int16_t sleep(bool retain) { mod->sleep_calls++; mod->retain = retain; return mod->sleep_error; }
  bool std_init() { mod->init_calls++; return mod->init_ok; }
  float getRSSI() { mod->spi_reads++; return -100; }
  float getSNR() { mod->spi_reads++; return 5; }
  virtual RadioLibTime_t getTimeOnAir(size_t) { return 1000; }
  RadioLibTime_t calculateTimeOnAir(int, DataRate_t, PacketConfig_t, size_t) { return 1000; }
};
class CustomSX1262 : public SX1262 {
public:
  CustomSX1262(Module* m) : SX1262(m) { }
  bool isReceiving() { return getIrqFlags() & 20; }
};
namespace mesh { struct MainBoard { }; }
struct PacketMillis { uint32_t preambleMillis, payloadMillis; };
class RadioLibWrapper {
protected:
  SX1262* _radio;
  int16_t _threshold = 0, _last_rx_error = 0;
  bool _cad_enabled = false, _interrupt = false;
  int _state = 0;
  virtual bool isReceivingPacket() = 0;
  virtual bool isReceivePacketReady() { return true; }
  virtual void onReceiveComplete() { }
  bool hasRadioInterrupt() const { return _interrupt; }
  bool isTransmitting() const { return _state == 3; }
  void markRadioIdle() { _state = 0; }
  void idle() { _radio->standby(); _state = 0; }
  void startRecv() { _last_rx_error = _radio->startReceive(); _state = _last_rx_error ? 0 : 1; }
public:
  RadioLibWrapper(SX1262& radio, mesh::MainBoard&) : _radio(&radio) { }
  virtual void setParams(float, float, uint8_t, uint8_t) = 0;
  virtual void loop() { }
  virtual int recvRaw(uint8_t*, int) { return 0; }
  virtual float getLastRSSI() const { return 0; }
  virtual float getLastSNR() const { return 0; }
  virtual float getCurrentRSSI() { return 0; }
  virtual bool getRxBoostedGainMode() const { return true; }
  virtual bool setRxBoostedGainMode(bool) { return true; }
  bool isInRecvMode() const { return _state == 1; }
  int getNoiseFloor() const { return -110; }
  void begin() { _state = 0; }
  void setTxPower(int8_t) { }
  static uint16_t preambleLengthForSF(uint8_t sf) { return sf <= 8 ? 32 : 16; }
  PacketMillis calcMaxPacketMillis(uint8_t, float, uint8_t, uint16_t) { return {150, 1000}; }
};
class CustomSX1262Wrapper : public RadioLibWrapper {
protected:
  bool isReceivingPacket() override { return ((CustomSX1262*)_radio)->isReceiving(); }
public:
  CustomSX1262Wrapper(CustomSX1262& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) { }
  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    _radio->freqMHz = freq; _radio->bandwidthKhz = bw; _radio->spreadingFactor = sf;
    _radio->codingRate = cr - 4;
    _radio->preambleLengthLoRa = preambleLengthForSF(sf);
  }
};
