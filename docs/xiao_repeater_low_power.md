# XIAO nRF52840 / Wio SX1262: Low-power repeater

This implementation is based on the official [repeater-v1.17.1](https://github.com/meshcore-dev/MeshCore/tree/repeater-v1.17.1) tag, commit `d92964352441e53b93e8667b802e04f6e072b39e`. The tested PlatformIO environment is `Xiao_nrf52_repeater`. The firmware identifies itself as `v1.17.1-lp1`.

The firmware has been built and passed software tests. The maintainer has also flashed it onto the target hardware and reported successful operation. See [reported hardware measurements](#reported-hardware-measurements) for current consumption and [hardware validation](#hardware-validation) for the broader test plan.

## Reported hardware measurements

The maintainer reported these approximate measurements on their XIAO nRF52840 / Wio SX1262 setup. During testing, the repeater was powered by a battery connected to the XIAO nRF52840 battery terminals, rather than by USB:

| Operating mode | Reported current |
| --- | ---: |
| RX duty cycling | 2.6 mA |
| Scheduled night sleep: SX1262 and nRF52840 sleeping | 0.5 mA (500 uA) |

These are measurements from one setup, not guaranteed specifications for every board. Supply voltage, USB connection, sensors, regulator losses, modem settings, RF traffic, and measurement method can affect the result. The successful flashing and operation report does not establish that every recovery and reliability test below has been completed.

## Implementation findings

- Entry point: `examples/simple_repeater/main.cpp`. Repeater behavior and admin commands: `MyMesh.cpp/.h`.
- `variants/xiao_nrf52/target.cpp` creates the radio, wrapper, and `AutoDiscoverRTCClock`, with `VolatileRTCClock` as a fallback.
- `src/helpers/NRF52Board.cpp::sleep()` uses System-ON sleep through `sd_app_evt_wait()` or WFE. The existing behavior and `powersaving on/off` commands are preserved.
- In the Arduino core used here (`meshcore-dev/Adafruit_nRF52_Arduino`, `d541301`), `millis()` comes from the FreeRTOS tick. Tickless idle uses RTC1 and retains RAM. Night sleep uses this existing mechanism without reconfiguring RTC registers.
- `VolatileRTCClock` accumulates `millis()` in `tick()`. The clock therefore continues to be updated at least once per second during night sleep.
- RadioLib is pinned to commit `6d8934836678d8894e3d556550475b37dce3e2b6`. Its SX126x API provides `calculateRxDutyCycle()`, `startReceiveDutyCycleAuto()`, `sleep(true)`, and `standby()` with wakeup.
- The original wrapper polls IRQ and RSSI and queries the modem over SPI to calculate airtime. Those accesses interfere with RX duty cycling. The new wrapper uses activity interrupts, caches packet RSSI/SNR before restarting RX, and calculates airtime without SPI access.
- `CommonCLI` already stores settings through `ConfigSerializer` in `/prefs.json`. This format is extended without deleting or rebuilding the identity, ACL, routing, or existing settings.
- Remote CLI commands use the existing decrypted admin path with replay checks. Successful `clock sync` and `time <Unix timestamp>` commands report the accepted time to the power manager. Neighbor adverts set neither the RTC nor clock validity.

## Operation and defaults

RX duty cycling and night sleep are **disabled** when no settings are present. The default night window is 01:00 to 05:00 **UTC**. Both options are independent of the existing CPU power-saving setting.

Normal CPU power saving keeps the configured RX mode active. Night sleep puts the SX1262 into warm sleep, pauses the mesh dispatcher, and blocks the loop task for at most one second at a time using `vTaskDelay()`. This lets the RTC1-based idle path sleep. Local serial diagnostics remain available. The new night path uses no multi-hour `delay()`, daily reboot, or SYSTEMOFF call.

`clock_valid` starts as `false` after every actual boot, even if an external RTC happens to contain a plausible year. Night sleep is allowed only after an accepted admin clock update with a plausible time and successful readback. Dates in 2024/2025 are rejected; the accepted range starts at 2026-01-01 and ends before 2100-01-01 UTC. A plausible date alone is insufficient.

The running clock is compared with elapsed `millis()` time. A backward jump, larger unexpected jump, or stalled clock invalidates it. Validity is retained after normal night sleep. Neither the time nor clock validity is stored in flash.

## RX duty cycling and preamble

RadioLib needs to know the shortest possible **transmitter preamble**. This MeshCore version transmits with 32 symbols at SF <= 8 and 16 symbols at higher spreading factors. Unknown or older transmitters must not be assumed to use 32 symbols universally.

`rx.preamble` therefore defaults conservatively to 16. At this length, continuous RX remains active. Only if all relevant transmitters actually send at least 32 preamble symbols at the selected SF should you configure the following for SF8/BW62.5:

```text
powersaving on
set rx.preamble 32
set rx.duty on
get rx.duty
```

`active=yes` indicates that duty cycling is active. Setting 32 when transmitters use shorter preambles can lose packets. For mixed or unknown transmitters, keep the value at 16 or leave duty cycling disabled.

The periods are calculated using the actual RadioLib function and current modem settings. Sleep periods that are too short, the wrong modem type, an unsuitable preamble, or startup errors cause a fallback to continuous RX. A failed duty-cycle start is not retried after every packet. Switching `off`/`on`, changing the preamble, or applying new radio parameters allows another attempt.

CAD or an active RSSI interference threshold also forces continuous RX. These features require radio activity that cannot be cleanly combined with uninterrupted duty-cycle sleep. Existing channel checks are therefore preserved.

A preamble interrupt only marks RX activity; it neither reads a packet nor restarts RX. Only RX_DONE invokes the existing packet processing. Header errors and timeouts restart reception. After TX, AGC reset, or radio recovery, the requested RX mode is restarted if supported. A bounded activity watchdog prevents the radio from remaining stuck after an incomplete preamble.

## CLI

All times and windows use **UTC in 24-hour `HH:MM` format**. Do not add AM or PM: `20:35` means 8:35 PM UTC. There is no automatic time-zone or daylight-saving conversion. Convert your local time to UTC before setting a window.

| Command | Effect |
| --- | --- |
| `powersaving on`, `powersaving off`, `powersaving` | Existing CPU power-saving controls |
| `get powersave` | Alias for CPU power-saving status |
| `get rx.duty` | Requested and active modes, transmitter preamble, CAD/RSSI policy, and last duty-cycle error |
| `set rx.duty on/off` | Save the requested duty-cycle setting; send either `on` or `off` |
| `get rx.preamble` | Show the minimum transmitter preamble |
| `set rx.preamble 16` or `set rx.preamble 32` | Set the minimum preamble; accepted range: 1..32 |
| `get night.sleep` | Show the setting, UTC window, and reason |
| `set night.sleep on/off` | Configure night sleep; send either `on` or `off` |
| `set night.start 01:00` | Set the start time |
| `set night.end 05:00` | Set the end time |
| `get night.start`, `get night.end` | Show window boundaries |
| `get clock.valid` | Show runtime clock validity |
| `clock` | Existing RTC time query |
| `clock sync` | Existing remote admin synchronization using the sender timestamp |
| `time <Unix timestamp>` | Existing explicit clock update, also available locally over USB |
| `get power.status` | Clock validity, Unix time, radio mode, scheduled wakeup, and radio errors |
| `get power.reason` | Reason for refusing sleep and the last wakeup reason |

The serial CLI has no sender timestamp, so `clock sync` cannot obtain the PC's time over USB. For a local clock update, obtain the current UTC Unix timestamp, for example using PowerShell `[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()`, and send `time <value>`. The existing rule allowing only forward time updates is preserved. Rejected synchronization attempts do not enable night sleep.

To test a two-minute window from **20:35 to 20:37 UTC**, send these commands individually before the window starts:

```text
set night.sleep off
set night.start 20:35
set night.end 20:37
```

Set or synchronize the clock using one of the methods above, then send:

```text
get clock.valid
set night.sleep on
get night.sleep
```

`get clock.valid` must return `true` for sleep to be allowed. The window repeats daily while enabled. Disable it after testing with `set night.sleep off`. During night sleep the repeater is unavailable over LoRa, while the local serial CLI remains accessible.

Equal start and end times form an invalid or empty window, not a 24-hour sleep period. Enabling such a window is rejected. Windows such as 21:00 to 05:00 work across midnight.

Only changed values of the new settings are saved. Status queries, date changes, sleep/wakeup, and clock synchronization write no new power or clock data to flash. Save errors are reported. The existing storage procedure is still non-transactional; these changes do not address a power failure while a settings file is being written.

## Sleep entry, wakeup, and fail-safes

Night sleep is considered only after a normal loop iteration. Every occupied slot in the existing packet pool prevents entry, including delayed RX, ongoing TX, and retained packets. Known radio interrupts or preambles, serial input, region import, pending ACL writes, neighbor discovery, and temporary radio changes also prevent entry. Pending work is processed first and can prevent sleep for the entire window.

A repeating FreeRTOS timer starts before the first night sleep. Entry is allowed only after it has delivered a callback and remains active with a recent callback. Timer failure prevents sleep or wakes the node at the next loop iteration. A second wakeup deadline uses monotonic `millis()` time and handles its 32-bit rollover. If this deadline must wake the node because the RTC has fallen behind, the clock is invalidated to prevent it from immediately sleeping again.

Wakeup uses `standby()` with NSS wakeup, followed by an RX start. On failure, the radio is reinitialized using the existing `std_init()`, restoring frequency, BW, SF, CR, preamble, TX power, CRC, and RX gain. Mesh identity and CPU uptime are preserved. Persistent RX errors are handled while the program continues running, with a limited retry rate and status reporting. Continuous RX is preferred after a radio error.

Timer and night-sleep errors prevent further night sleep until an explicit configuration command, such as `set night.sleep on`, resets the error latch. An invalid clock still requires an accepted clock update. The saved night-sleep setting may remain `on` after reboot, but sleep is effectively blocked while `clock_valid=false`.

Diagnostics are provided on request. Optional `MESH_DEBUG` output occurs on transitions and errors, not every second during normal operation.

## Limitations

- The SX1262 has no atomic command for conditional sleep when radio activity starts simultaneously. Known RX/TX activity is allowed to finish and checked again immediately before sleep. A packet starting between the final check and the sleep command can be lost at the boundary of the deliberately offline window. This API cannot guarantee zero packet loss at that boundary.
- The firmware cannot guarantee the preamble length of arbitrary third-party transmitters. The configured minimum must match the entire relevant group of transmitters.
- RadioLib and timer errors are handled while the CPU is still running. An RTC1 or core scheduler failure, CPU lockup, or permanently faulty radio hardware cannot be fully covered without an independent hardware wakeup or watchdog. No new reboot watchdog has been added.
- The XIAO's existing undervoltage boot protection still uses SYSTEMOFF. This board protection path is unchanged and is separate from normal night or CPU sleep.
- USB, sensors, board regulators, RX gain, the RF switch, and the TCXO affect actual current consumption. The measurements above apply to the maintainer's setup and are not universal current targets.

## Build and software tests

With PlatformIO installed, run these commands from the repository root:

```text
pio run -e Xiao_nrf52_repeater
pio run -e Xiao_nrf52_repeater -t create_uf2
```

The UF2 converter handles paths containing spaces and returns its errors to SCons.

The original baseline, RX-duty intermediate version, and final firmware all built successfully:

| Version | Flash | RAM |
| --- | ---: | ---: |
| Unmodified baseline | 387,720 bytes | 31,160 bytes |
| RX-duty intermediate version | 390,568 bytes | 31,192 bytes |
| Final: `v1.17.1-lp1` | 397,864 bytes | 31,296 bytes |

The baseline and intermediate builds were saved locally under `../artifacts/baseline` and `../artifacts/rx-duty`. The intermediate version assumes a group of transmitters using 32-symbol preambles and lacks the final night-sleep and preamble fail-safes. Use the final firmware for further testing; duty cycling and night sleep can be enabled separately through the CLI. These local artifacts and the tools below are outside the firmware repository and are not required for a normal PlatformIO build.

The 17 C++ tests compile the actual power policy and new radio wrapper for Cortex-M4 and run under Unicorn. The radio API is mocked for targeted error and state checks. Coverage includes window boundaries, 7,200 minute/window combinations, boot and default clock state, trusted clock updates and readback, clock jumps and stalls, timer failures, a five-minute cycle, repeated nights, `millis()` rollover, duty-cycle fallback, preamble handling, idle operation without SPI, TX and pending-work guards, warm sleep, and recovery.

To run the tests, install their Python dependencies and provide an ARM G++ compiler:

```text
python -m pip install -r tests/requirements-power.txt
python tests/run_power_tests.py --compiler /path/to/arm-none-eabi-g++
```

On Windows, use the compiler's `.exe` path. The runner also accepts the `ARM_CXX` environment variable. Without either override, it uses the compiler in the original local `../.tools/platformio` installation.

For the original local Windows build setup, PlatformIO, Python, and packages were installed under `../.tools`. The following commands remain available from this repository when that directory exists:

```powershell
$env:PLATFORMIO_CORE_DIR = Join-Path (Split-Path (Get-Location)) '.tools\platformio'
& '..\.tools\pio-venv\Scripts\platformio.exe' run -e Xiao_nrf52_repeater
& '..\.tools\pio-venv\Scripts\platformio.exe' run -e Xiao_nrf52_repeater -t create_uf2
& '..\.tools\pio-venv\Scripts\python.exe' tests\run_power_tests.py
```

## Hardware validation

Flashing, operation, and the current measurements above have been reported by the maintainer. The following is the broader hardware validation plan; completion of every item has not been reported:

1. Test the final version with both new options disabled: RX/TX, admin commands, flood and direct traffic, and the existing `powersaving` behavior.
2. Enable RX duty cycling only with a known transmitter preamble. Compare packet loss against the baseline, verify RX after TX, AGC reset, and parameter changes, and measure current. Also check continuous-RX fallback for different SF/BW/preamble settings, CAD, and interference thresholds.
3. Set the clock through the admin path and check `get clock.valid`. Configure a five-minute UTC window starting about two minutes in the future. Verify sleep, current, automatic wakeup, unchanged uptime and identity, and admin/flood/direct operation after wakeup. Repeat several times.
4. Perform an actual reboot with night sleep saved as enabled. `get clock.valid` must return `false`, and the repeater must remain online during the window. After an accepted clock sync, it must return `true`, allowing sleep again. Default dates in 2024/2025 must never enable sleep.
5. Test local `set night.sleep off` during sleep and pending TX/delayed RX at the window boundary. Validate these cases before relying on a multi-hour window.

## Changed files

| File | Change |
| --- | --- |
| `variants/xiao_nrf52/platformio.ini`, `target.h` | New radio and wrapper only for this repeater environment; identifiable firmware version |
| `src/helpers/radiolib/RepeaterSX1262Wrapper.h` (new) | Duty cycling, IRQ handling, warm sleep, recovery, and deferred radio parameter changes |
| `src/helpers/radiolib/RadioLibWrappers.h/.cpp` | Small hooks for preamble versus packet interrupts, metadata, and state/RX error queries |
| `examples/simple_repeater/RepeaterPowerManager.h` (new) | Clock trust, UTC windows, and two wakeup deadlines |
| `examples/simple_repeater/MyMesh.h/.cpp` | Admin CLI, persistence, pending-work checks before night sleep, and power transitions |
| `examples/simple_repeater/main.cpp` | RTC1/FreeRTOS timer, night loop, and local serial access |
| `src/helpers/CommonCLI.h/.cpp` | Power preferences and hook after accepted clock updates; compatible with existing preferences |
| `create-uf2.py` | Correct path quoting and error propagation |
| `tests/` | Executable C++ policy/API tests and runner |

`CustomSX1262.h`, `CustomSX1262Wrapper.h`, `NRF52Board.cpp`, routing, keys, and neighbor time behavior remain as in the official source version. The new wrappers reuse these existing implementations.
