#include "../examples/simple_repeater/RepeaterPowerManager.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
using Power = RepeaterPowerManager;
static constexpr uint32_t DAY = 1767225600UL; // 2026-01-01 00:00 UTC

extern "C" int test_boot_and_trust() {
  Power p;
  CHECK(!p.clockValid());
  CHECK(p.update(DAY + 7200, 0, true, 60, 300, false, true) == Power::NONE);
  p.trustedClockSet(1715770351, 1715770351, 0);
  CHECK(!p.clockValid());
  p.trustedClockSet(DAY - 1, DAY - 1, 0);
  CHECK(!p.clockValid());
  p.trustedClockSet(DAY, DAY + 3, 0);
  CHECK(!p.clockValid());
  p.trustedClockSet(DAY, DAY - 1, 0);
  CHECK(!p.clockValid());
  p.trustedClockSet(4102444800UL, 4102444800UL, 0);
  CHECK(!p.clockValid());
  p.trustedClockSet(DAY, DAY + 1, 0);
  CHECK(p.clockValid());
  Power reboot;
  CHECK(!reboot.clockValid());
  return 0;
}

extern "C" int test_window_boundaries() {
  CHECK(!Power::validWindow(60, 60));
  CHECK(!Power::validWindow(1440, 60));
  CHECK(!Power::validWindow(60, 65535));
  CHECK(!Power::inWindow(DAY + 3599, 60, 300));
  CHECK(Power::inWindow(DAY + 3600, 60, 300));
  CHECK(Power::inWindow(DAY + 17999, 60, 300));
  CHECK(!Power::inWindow(DAY + 18000, 60, 300));
  CHECK(Power::windowEnd(DAY + 3600, 300) == DAY + 18000);
  CHECK(!Power::inWindow(DAY + 75599, 1260, 300));
  CHECK(Power::inWindow(DAY + 75600, 1260, 300));
  CHECK(Power::inWindow(DAY + 86399, 1260, 300));
  CHECK(Power::inWindow(DAY + 86400, 1260, 300));
  CHECK(!Power::inWindow(DAY + 104400, 1260, 300));
  CHECK(Power::windowEnd(DAY + 75600, 300) == DAY + 104400);
  CHECK(Power::windowEnd(DAY + 86400, 300) == DAY + 104400);
  CHECK(Power::windowEnd(DAY + 86399, 0) == DAY + 86400);
  return 0;
}

extern "C" int test_all_minutes() {
  // Compare the implementation with elapsed time around a 24-hour clock.
  const uint16_t windows[][2] = {{0, 1}, {60, 300}, {1260, 300}, {1439, 0}, {300, 299}};
  for (auto& w : windows) {
    uint32_t span = (w[1] + 1440 - w[0]) % 1440;
    for (uint32_t minute = 0; minute < 1440; minute++) {
      bool expected = (minute + 1440 - w[0]) % 1440 < span;
      CHECK(Power::inWindow(DAY + minute * 60, w[0], w[1]) == expected);
      if (expected) {
        auto end = Power::windowEnd(DAY + minute * 60, w[1]);
        CHECK(end > DAY + minute * 60);
        CHECK(end - (DAY + minute * 60) <= 86400);
        CHECK(!Power::inWindow(end, w[0], w[1]));
      }
    }
  }
  return 0;
}

extern "C" int test_entry_guards() {
  Power p;
  p.trustedClockSet(DAY + 7200, DAY + 7200, 0);
  CHECK(p.update(DAY + 7200, 0, false, 60, 300, false, true) == Power::NONE);
  CHECK(p.update(DAY + 7200, 0, true, 60, 60, false, true) == Power::NONE);
  CHECK(p.update(DAY + 7200, 0, true, 60, 300, true, true) == Power::NONE);
  CHECK(p.update(DAY + 7200, 0, true, 60, 300, false, false) == Power::NONE);
  CHECK(p.update(DAY + 7200, 0, true, 60, 300, false, true) == Power::SLEEP);
  CHECK(!p.sleeping()); // entry is only committed after the radio accepted sleep
  p.denySleep("radio sleep error");
  CHECK(p.update(DAY + 7200, 0, true, 60, 300, false, true) == Power::NONE);
  p.resetFault();
  CHECK(p.update(DAY + 7200, 0, true, 60, 300, false, true) == Power::SLEEP);
  return 0;
}

extern "C" int test_five_minute_sleep() {
  Power p;
  auto start = DAY + 20 * 3600 + 120;
  p.trustedClockSet(start, start, 100);
  CHECK(p.update(start, 100, true, 1202, 1207, false, true) == Power::SLEEP);
  p.confirmSleep(start, 100, 1207);
  CHECK(p.nextWake() == DAY + 20 * 3600 + 420);
  for (uint32_t second = 1; second < 300; second++) {
    CHECK(p.update(start + second, 100 + second * 1000, true, 1202, 1207, false, true) == Power::NONE);
    CHECK(p.sleeping() && p.clockValid());
  }
  CHECK(p.update(start + 300, 300100, true, 1202, 1207, false, true) == Power::WAKE);
  CHECK(!p.sleeping() && p.clockValid());
  CHECK(p.nextWake() == 0);
  CHECK(p.update(start + 301, 301100, true, 1202, 1207, false, true) == Power::NONE);
  return 0;
}

extern "C" int test_repeated_nights_and_rollover() {
  Power p;
  uint32_t ms = 0xFFFFFF00UL;
  uint32_t now = DAY + 75600;
  p.trustedClockSet(now, now, ms);
  for (int day = 0; day < 3; day++) {
    CHECK(p.update(now, ms, true, 1260, 300, false, true) == Power::SLEEP);
    p.confirmSleep(now, ms, 300);
    now += 28800;
    ms += 28800000;
    CHECK(p.update(now, ms, true, 1260, 300, false, true) == Power::WAKE);
    CHECK(p.clockValid());
    now += 57600;
    ms += 57600000;
  }
  return 0;
}

extern "C" int test_timer_failure_wakes_and_latches() {
  Power p;
  auto now = DAY + 7200;
  p.trustedClockSet(now, now, 0);
  p.confirmSleep(now, 0, 300);
  CHECK(p.update(now + 1, 1000, true, 60, 300, false, false) == Power::WAKE);
  CHECK(!p.sleeping() && p.clockValid());
  CHECK(p.update(now + 2, 2000, true, 60, 300, false, true) == Power::NONE);
  p.resetFault();
  CHECK(p.update(now + 2, 2000, true, 60, 300, false, true) == Power::SLEEP);
  return 0;
}

extern "C" int test_clock_failure_wakes() {
  Power p;
  auto now = DAY + 7200;
  p.trustedClockSet(now, now, 0);
  p.confirmSleep(now, 0, 300);
  CHECK(p.update(now - 60, 1000, true, 60, 300, false, true) == Power::WAKE);
  CHECK(!p.clockValid());
  CHECK(p.update(now, 2000, true, 60, 300, false, true) == Power::NONE);
  p.trustedClockSet(now, now, 2000);
  CHECK(p.update(now, 2000, true, 60, 300, false, true) == Power::SLEEP);
  p.confirmSleep(now, 2000, 300);
  CHECK(p.update(now + 60, 3000, true, 60, 300, false, true) == Power::WAKE);
  CHECK(!p.clockValid());
  return 0;
}

extern "C" int test_stalled_clock_and_monotonic_deadline() {
  Power p;
  auto now = DAY + 7200;
  p.trustedClockSet(now, now, 0);
  p.confirmSleep(now, 0, 300);
  CHECK(p.update(now, 1000, true, 60, 300, false, true) == Power::NONE);
  CHECK(p.update(now, 2000, true, 60, 300, false, true) == Power::NONE);
  CHECK(p.update(now, 3000, true, 60, 300, false, true) == Power::WAKE);
  CHECK(!p.clockValid());
  Power deadline;
  now = DAY + 17999;
  deadline.trustedClockSet(now, now, 0);
  deadline.confirmSleep(now, 0, 300);
  CHECK(deadline.update(now, 1000, true, 60, 300, false, true) == Power::WAKE);
  CHECK(!deadline.clockValid());
  CHECK(deadline.update(now, 1000, true, 60, 300, false, true) == Power::NONE);
  return 0;
}

extern "C" int test_disable_and_pending_wake() {
  auto now = DAY + 7200;
  Power p;
  p.trustedClockSet(now, now, 0);
  p.confirmSleep(now, 0, 300);
  CHECK(p.update(now, 0, false, 60, 300, false, true) == Power::WAKE);
  CHECK(p.clockValid());
  p.confirmSleep(now, 0, 300);
  CHECK(p.update(now, 0, true, 60, 300, true, true) == Power::WAKE);
  p.confirmSleep(now, 0, 300);
  CHECK(p.update(now, 0, true, 900, 1000, false, true) == Power::WAKE);
  CHECK(p.clockValid());
  return 0;
}
