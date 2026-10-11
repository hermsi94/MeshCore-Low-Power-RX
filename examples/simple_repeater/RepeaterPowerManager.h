#pragma once

#include <stdint.h>

class RepeaterPowerManager {
  bool _clock_valid = false, _sleeping = false, _fault = false;
  uint32_t _trusted_time = 0, _last_ms = 0;
  uint64_t _elapsed_ms = 0;
  uint32_t _wake_time = 0, _sleep_ms = 0, _sleep_duration_ms = 0;
  const char* _reason = "clock invalid";
  const char* _wake_reason = "boot";

public:
  enum Decision { NONE, SLEEP, WAKE };
  static bool plausibleTime(uint32_t time) {
    return time >= 1767225600UL && time < 4102444800UL; // 2026-01-01 .. 2100-01-01 UTC
  }
  static bool validWindow(uint16_t start, uint16_t end) {
    return start < 1440 && end < 1440 && start != end;
  }
  static bool inWindow(uint32_t time, uint16_t start, uint16_t end) {
    if (!validWindow(start, end)) return false;
    uint16_t minute = (time % 86400UL) / 60;
    return start < end ? minute >= start && minute < end : minute >= start || minute < end;
  }
  static uint32_t windowEnd(uint32_t time, uint16_t end) {
    uint32_t remaining = (end * 60UL + 86400UL - time % 86400UL) % 86400UL;
    return time + remaining;
  }
  bool clockValid() const { return _clock_valid; }
  bool sleeping() const { return _sleeping; }
  uint32_t nextWake() const { return _sleeping ? _wake_time : 0; }
  const char* reason() const { return _reason; }
  const char* wakeReason() const { return _wake_reason; }

  void trustedClockSet(uint32_t accepted, uint32_t actual, uint32_t ms) {
    _clock_valid = plausibleTime(accepted) && actual >= accepted && actual - accepted <= 2;
    _trusted_time = actual;
    _last_ms = ms;
    _elapsed_ms = 0;
  }
  void observeClock(uint32_t time, uint32_t ms) {
    if (!_clock_valid) return;
    _elapsed_ms += uint32_t(ms - _last_ms);
    _last_ms = ms;
    int64_t drift = int64_t(time) - _trusted_time - int64_t(_elapsed_ms / 1000);
    if (!plausibleTime(time) || drift < -2 || drift > 2) _clock_valid = false;
  }
  void resetFault() { _fault = false; }
  void deferSleep(const char* reason) { _reason = reason; }
  void denySleep(const char* reason) {
    _fault = true;
    _reason = reason;
  }
  void confirmSleep(uint32_t time, uint32_t ms, uint16_t end) {
    _wake_time = windowEnd(time, end);
    _sleep_ms = ms;
    _sleep_duration_ms = (_wake_time - time) * 1000UL;
    _sleeping = true;
    _reason = "night sleep";
  }
  void cancelSleep(const char* reason) {
    _sleeping = false;
    _wake_reason = reason;
  }
  Decision update(uint32_t time, uint32_t ms, bool enabled, uint16_t start, uint16_t end,
                  bool pending, bool timer_ok) {
    observeClock(time, ms);
    const char* refusal = nullptr;
    if (!enabled) refusal = "disabled";
    else if (!_clock_valid) refusal = "clock invalid";
    else if (!validWindow(start, end)) refusal = "invalid window";
    else if (!timer_ok) refusal = "timer fault";
    else if (_fault) refusal = _reason;
    else if (!inWindow(time, start, end)) refusal = "outside window";

    if (_sleeping) {
      bool deadline = uint32_t(ms - _sleep_ms) >= _sleep_duration_ms || time >= _wake_time;
      if (refusal || pending || deadline) {
        bool monotonic_fallback = deadline && time < _wake_time;
        if (monotonic_fallback) _clock_valid = false;
        cancelSleep(monotonic_fallback ? "monotonic fallback" :
          (deadline ? "scheduled wake" : (refusal ? refusal : "pending work")));
        if (!timer_ok) denySleep("timer fault");
        return WAKE;
      }
      return NONE;
    }
    if (refusal) {
      if (!_fault) _reason = refusal;
      return NONE;
    }
    _reason = pending ? "pending work" : "ready";
    return pending ? NONE : SLEEP;
  }
};
