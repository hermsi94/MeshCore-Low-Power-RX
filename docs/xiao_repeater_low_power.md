# XIAO nRF52840 / Wio SX1262: Repeater mit Stromsparfunktionen

Ausgangsbasis ist der offizielle Tag [repeater-v1.17.1](https://github.com/meshcore-dev/MeshCore/tree/repeater-v1.17.1), Commit `d92964352441e53b93e8667b802e04f6e072b39e`. Das gepruefte PlatformIO-Ziel heisst `Xiao_nrf52_repeater`. Diese Anpassung meldet sich als `v1.17.1-lp1`.

Die Implementierung wurde gebaut und mit Softwaretests geprueft. Sie wurde hier **nicht auf einen Repeater geflasht**. Funkzuverlaessigkeit, Stromverbrauch und physisches Aufwachen sind noch auf der Zielhardware zu pruefen.

## Ergebnisse der Untersuchung

- Entry Point: `examples/simple_repeater/main.cpp`, Repeater und Admin-Kommandos: `MyMesh.cpp/.h`.
- `variants/xiao_nrf52/target.cpp` erzeugt Radio, Wrapper und `AutoDiscoverRTCClock` mit `VolatileRTCClock` als Fallback.
- `src/helpers/NRF52Board.cpp::sleep()` verwendet System-ON mit `sd_app_evt_wait()` bzw. WFE. Die bisherige Funktion und `powersaving on/off` bleiben erhalten.
- Im verwendeten Arduino-Kern (`meshcore-dev/Adafruit_nRF52_Arduino`, `d541301`) kommt `millis()` aus dem FreeRTOS-Tick. Tickless Idle verwendet RTC1 und behaelt RAM. Dieser vorhandene Mechanismus traegt den Nachtmodus; es wird kein eigenes RTC-Register umkonfiguriert.
- `VolatileRTCClock` akkumuliert `millis()` in `tick()`. Deshalb wird die Uhr auch waehrend der Nacht mindestens einmal pro Sekunde aktualisiert.
- RadioLib ist auf Commit `6d8934836678d8894e3d556550475b37dce3e2b6` festgelegt. Dessen SX126x-API bietet `calculateRxDutyCycle()`, `startReceiveDutyCycleAuto()`, `sleep(true)` und `standby()` mit Wakeup.
- Der vorhandene Wrapper pollt IRQ und RSSI und fragt fuer Airtime das Modem per SPI ab. Solche Zugriffe sind fuer RX Duty Cycle ungeeignet. Der neue Wrapper verwendet Aktivitaets-Interrupts, speichert Paket-RSSI/SNR vor dem RX-Neustart und berechnet Airtime ohne SPI.
- `CommonCLI` speichert Einstellungen bereits ueber `ConfigSerializer` in `/prefs.json`. Dieses Format wird erweitert; Identity, ACL, Routing und bestehende Einstellungen werden nicht geloescht oder neu aufgebaut.
- Remote-CLI-Kommandos kommen ueber den vorhandenen entschluesselten Admin-Pfad mit Replay-Pruefung. Erfolgreiches `clock sync` und `time <Unixzeit>` melden die akzeptierte Zeit an den Power Manager. Nachbar-Adverts setzen weder die RTC noch die Uhrvaliditaet.

## Betrieb und Voreinstellungen

RX Duty Cycle und Night Sleep sind bei fehlenden Einstellungen **ausgeschaltet**. Das Nachtfenster ist vorbelegt mit 01:00 bis 05:00 **UTC**. Beide Optionen sind unabhaengig von der bestehenden CPU-Power-Saving-Einstellung.

Die normale CPU-Stromsparfunktion laesst den konfigurierten RX-Modus aktiv. Der Nachtmodus setzt den SX1262 in Warm Sleep, pausiert den Mesh-Dispatcher und blockiert den Loop-Task jeweils hoechstens eine Sekunde mit `vTaskDelay()`. Der RTC1-basierte Idle-Pfad kann damit schlafen. Lokale serielle Diagnose bleibt moeglich. Es gibt keinen mehrstuendigen `delay()`, keinen taeglichen Reboot und keinen SYSTEMOFF-Aufruf im neuen Nachtpfad.

`clock_valid` startet bei jedem echten Boot mit `false`, auch wenn eine externe RTC zufaellig eine plausible Jahreszahl liefern sollte. Erst eine akzeptierte Admin-Uhrsetzung mit plausibler Zeit und erfolgreicher Ruecklesung gibt den Nachtmodus frei. 2024/2025 werden abgewiesen; die Plausibilitaetsgrenzen sind 2026-01-01 bis vor 2100-01-01 UTC. Ein plausibles Datum alleine reicht nicht.

Die laufende Uhr wird mit verstrichener `millis()`-Zeit verglichen. Ein Ruecksprung, groesserer unerwarteter Sprung oder Stillstand entzieht die Validitaet. Nach normalem Nachtschlaf bleibt sie erhalten. Es wird keine Uhrzeit und keine Validitaet in Flash gespeichert.

## RX Duty Cycle und Praeambel

RadioLib muss wissen, wie kurz die **Senderpraeambel** sein kann. Diese MeshCore-Version sendet bei SF <= 8 mit 32 Symbolen, darueber mit 16. Fuer unbekannte oder aeltere Sender darf daraus keine universelle 32-Symbol-Annahme abgeleitet werden.

`rx.preamble` ist deshalb konservativ mit 16 vorbelegt. Bei dieser Laenge bleibt Continuous RX aktiv. Erst wenn alle relevanten Sender bei der verwendeten SF tatsaechlich mindestens 32 Praeambelsymbole senden, kann man fuer SF8/BW62.5 konfigurieren:

```text
powersaving on
set rx.preamble 32
set rx.duty on
get rx.duty
```

`active=yes` zeigt den aktiven Duty-Cycle-Modus. Eine Einstellung von 32 trotz kuerzerer Senderpraeambeln kann Pakete verlieren. Bei gemischten oder unbekannten Sendern bei 16 bleiben bzw. Duty Cycle ausgeschaltet lassen.

Die Periodenberechnung erfolgt mit der echten RadioLib-Funktion und den aktuellen Modemwerten. Zu kurze Schlafperioden, falscher Modemtyp, unpassende Praeambel oder Startfehler fuehren zu Continuous RX. Ein fehlgeschlagener Duty-Cycle-Start wird nicht nach jedem Paket wieder versucht. `off`/`on`, eine geaenderte Praeambel oder neue Funkparameter erlauben einen neuen Versuch.

CAD oder eine aktive RSSI-Interferenzschwelle erzwingen ebenfalls Continuous RX. Diese Funktionen brauchen Radioaktivitaet, die sich mit ungestoertem Duty-Cycle-Schlaf nicht sauber kombinieren laesst. Die vorhandene Kanalpruefung bleibt damit erhalten.

Ein Praeambel-Interrupt markiert nur RX-Aktivitaet. Er liest kein Paket und startet RX nicht neu. Erst RX_DONE fuehrt zur vorhandenen Paketverarbeitung. Headerfehler/Timeouts starten den Empfang erneut. Nach TX, AGC-Reset und Radio-Recovery wird der gewuenschte, unterstuetzte RX-Modus wieder gestartet. Ein zeitlich begrenzter Aktivitaets-Watchdog verhindert Festhaengen nach unvollstaendigen Praeambeln.

## CLI

Alle Uhrzeiten und Zeitfenster sind UTC. Es gibt keine automatische Sommer-/Winterzeitumrechnung.

| Kommando | Wirkung |
| --- | --- |
| `powersaving on`, `powersaving off`, `powersaving` | Bestehende CPU-Funktion |
| `get powersave` | Alias fuer den CPU-Status |
| `get rx.duty` | Wunsch, aktiver Modus, Senderpraeambel, CAD/RSSI-Policy, letzter Duty-Fehler |
| `set rx.duty on/off` | Duty-Cycle-Wunsch speichern |
| `get rx.preamble` | Mindestpraeambel der Sender anzeigen |
| `set rx.preamble 16` bzw. `32` | Mindestpraeambel einstellen; akzeptierter Bereich 1..32 |
| `get night.sleep` | Einstellung, UTC-Fenster und Grund anzeigen |
| `set night.sleep on/off` | Nachtmodus konfigurieren |
| `set night.start 01:00` | Beginn einstellen |
| `set night.end 05:00` | Ende einstellen |
| `get night.start`, `get night.end` | Grenzen anzeigen |
| `get clock.valid` | Laufzeit-Uhrvaliditaet anzeigen |
| `clock` | Bestehende RTC-Zeitabfrage |
| `clock sync` | Bestehende Remote-Admin-Synchronisation ueber den Sender-Timestamp |
| `time <Unixsekunden>` | Bestehende explizite Uhrsetzung, auch lokal per USB |
| `get power.status` | Validitaet, Unixzeit, Radio-Modus, geplanter Wake und Radiofehler |
| `get power.reason` | Sleep-Verweigerung und letzter Wake-Grund |

Die serielle CLI hat keinen Sender-Timestamp: Dort kann `clock sync` keine Zeit vom PC beziehen. Fuer eine lokale Uhrsetzung den aktuellen UTC-Unix-Timestamp verwenden, z.B. mit PowerShell `[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()`, und dann `time <Wert>` senden. Die bestehende Vorwaerts-only-Regel bleibt erhalten. Abgelehnte Synchronisationen geben den Nachtmodus nicht frei.

Gleiche Start-/Endzeit ist ein ungueltiges bzw. leeres Fenster, kein 24-Stunden-Schlaf. Beim Einschalten wird es abgewiesen. Fenster wie 21:00 bis 05:00 funktionieren ueber Mitternacht.

Nur geaenderte neue Konfigurationswerte werden gespeichert. Statusabfragen, Tageswechsel, Sleep/Wake und Clock Sync schreiben keine neuen Power-/Clock-Daten in Flash. Fehler beim Speichern werden gemeldet. Das bereits vorhandene Speicherverfahren ist weiterhin nicht transaktional; ein Stromausfall mitten im Schreiben einer Einstellungsdatei ist kein durch diese Anpassung geloestes Problem.

## Eintritt, Wake und Failsafes

Der Nachtmodus wird erst nach einem normalen Loop-Durchlauf betrachtet. Alle belegten Plaetze des vorhandenen Paketpools verhindern den Eintritt, also auch verzogerter RX, laufende TX und gehaltene Pakete. Dazu kommen bekannte Radio-Interrupts/Praeambeln, serielle Eingaben, Region-Import, ausstehende ACL-Schreibvorgaenge, Neighbor-Discovery und temporaere Radioaenderungen. Arbeit wird zuerst abgearbeitet; sie kann das Nachtfenster ganz verhindern.

Ein wiederholender FreeRTOS-Timer wird vor dem ersten Nachtschlaf gestartet. Eintritt ist erst erlaubt, wenn er einen Callback geliefert hat und noch aktiv/frisch ist. Sein Ausfall verhindert Sleep oder weckt beim naechsten Loop-Durchlauf. Eine zweite Wake-Grenze verwendet monotone `millis()`-Zeit und funktioniert auch ueber deren 32-Bit-Ueberlauf. Muss diese Grenze wegen einer zurueckliegenden RTC-Zeit greifen, wird die Uhr ungueltig, um erneutes Einschlafen zu verhindern.

Wake erfolgt ueber `standby()` mit NSS-Wakeup und anschliessenden RX-Start. Bei Fehler folgt eine Radio-Reinitialisierung mit dem vorhandenen `std_init()` sowie Wiederherstellung von Frequenz, BW, SF, CR, Praeambel, TX-Leistung, CRC und RX-Gain. Dabei bleiben Mesh-Identitaet und CPU-Laufzeit erhalten. Fortbestehende RX-Fehler werden im laufenden Programm mit begrenzter Retry-Frequenz behandelt und im Status gemeldet. Nach einem Radiofehler wird Continuous RX bevorzugt.

Timer- und Night-Sleep-Fehler sperren weiteren Nachtschlaf bis zur erneuten bewussten Konfiguration, z.B. `set night.sleep on`. Eine ungueltige Uhr benoetigt weiterhin eine akzeptierte Uhrsetzung. Die Nacht-Einstellung kann nach Reboot gespeichert `on` bleiben; effektiv ist Sleep mit `clock_valid=false` gesperrt.

Diagnose erfolgt auf Anfrage. Optionale `MESH_DEBUG`-Ausgaben entstehen bei Uebergaengen/Fehlern, nicht sekundenweise im normalen Betrieb.

## Grenzen gegenueber den Anforderungen

- Der SX1262 hat keinen atomaren Befehl fuer bedingtes Einschlafen bei gleichzeitigem Funkbeginn. Bekannte RX/TX-Aktivitaet wird abgewartet und direkt vor Sleep erneut geprueft. Ein neu beginnendes Paket unmittelbar zwischen letzter Pruefung und Sleep-Befehl kann an der Grenze des bewusst offline geschalteten Fensters verloren gehen. Eine absolute Nullverlust-Garantie an dieser Grenze ist mit dieser API nicht moeglich.
- Eine Praeambelgarantie fuer beliebige fremde Sender kann die Firmware nicht ableiten. Die Mindestlaenge muss zur gesamten relevanten Sendergruppe passen.
- RadioLib-/Timer-Fehler mit noch laufender CPU werden abgefangen. Ein ausgefallener RTC1/Kern-Scheduler, ein CPU-Lockup oder dauerhaft defekte Funkhardware lassen sich ohne einen unabhaengigen Hardware-Wake/Watchdog nicht absolut absichern. Hier wurde kein neuer Reboot-Watchdog eingefuehrt.
- Die bestehende Unterspannungs-Bootsperre des XIAO nutzt weiterhin SYSTEMOFF. Das ist ein unveraenderter Schutzpfad des Boards und nicht der neue normale Nacht-/CPU-Schlaf.
- USB, Sensoren, Board-Regler, RX-Gain, RF-Switch und TCXO beeinflussen den realen Strombedarf. 4..6 mA bzw. ein bestimmter Nachtstrom werden nicht als erreicht behauptet.

## Build und Teststand

PlatformIO/Python und Pakete wurden lokal unter `../.tools` installiert. Aus diesem Repository:

```powershell
$env:PLATFORMIO_CORE_DIR = Join-Path (Split-Path (Get-Location)) '.tools\platformio'
& '..\.tools\pio-venv\Scripts\platformio.exe' run -e Xiao_nrf52_repeater
& '..\.tools\pio-venv\Scripts\platformio.exe' run -e Xiao_nrf52_repeater -t create_uf2
& '..\.tools\pio-venv\Scripts\python.exe' tests\run_power_tests.py
```

Der UF2-Konverter wurde fuer Pfade mit Leerzeichen korrigiert. Seine Fehler werden nun an SCons zurueckgegeben.

Baseline und RX-Duty-Zwischenstand wurden separat erfolgreich gebaut und unter `../artifacts/baseline` bzw. `../artifacts/rx-duty` gesichert. Der Zwischenstand ist fuer die 32-Symbol-Sendergruppe gedacht und enthaelt noch nicht die abschliessenden Nacht-/Praeambel-Failsafes. Fuer die weitere Erprobung den finalen Stand verwenden; Duty und Nachtmodus lassen sich einzeln per CLI aktivieren.

17 C++-Tests kompilieren die echte Power-Policy und den neuen Radio-Wrapper fuer Cortex-M4 und laufen unter Unicorn. Die Radio-API wird dabei fuer gezielte Fehler- und Zustandspruefungen gemockt. Abgedeckt sind Fenstergrenzen, 7.200 Minuten-/Fensterkombinationen, Reboot/Default-Uhr, vertraute Uhrsetzung/Ruecklesung, Uhrspruenge/Stillstand, Timerfehler, ein 5-Minuten-Zyklus, wiederholte Naechte, `millis()`-Ueberlauf, Duty-Fallback, Praeambelbehandlung, SPI-freies Idle, TX-/Pending-Sperren, Warm Sleep und Recovery. Tests benoetigen `pyelftools` und `unicorn==2.1.4`; alternativ kann `--compiler` einen anderen ARM-G++ angeben.

Auf der Hardware noch durchzufuehren:

1. Finalen Stand mit beiden Optionen aus pruefen: RX/TX, Admin, Flood, Direct und vorhandenes `powersaving`.
2. Bei bekannter Senderpraeambel erst RX Duty aktivieren. Paketverluste gegen den Baseline-Stand vergleichen, RX nach TX/AGC/Parameterwechsel pruefen und Strom messen. Mit SF/BW/Praeambel, CAD und Interferenzschwelle auch Continuous-Fallback pruefen.
3. Uhr per Admin setzen, `get clock.valid` pruefen. Ein UTC-Fenster etwa zwei Minuten in der Zukunft mit fuenf Minuten Dauer einstellen. Sleep, Strom, selbststaendigen Wake, unveraenderte Uptime/Identitaet und Admin/Flood/Direct nach Wake pruefen. Mehrfach wiederholen.
4. Echter Reboot mit gespeichertem Nachtmodus: `get clock.valid` muss `false` sein; der Repeater muss waehrend des Fensters online bleiben. Danach akzeptierter Clock Sync: `true`, Sleep wieder moeglich. Default-Zeit 2024/2025 darf nie freigeben.
5. Lokales `set night.sleep off` waehrend Sleep sowie ausstehende TX/Delayed-RX am Fensterbeginn pruefen. Erst nach diesen Versuchen ein echtes mehrstuendiges Fenster einstellen.

## Geaenderte Stellen

| Datei | Aenderung |
| --- | --- |
| `variants/xiao_nrf52/platformio.ini`, `target.h` | Neues Radio/Wrapper nur fuer dieses Repeater-Ziel, erkennbare Firmware-Version |
| `src/helpers/radiolib/RepeaterSX1262Wrapper.h` (neu) | Duty-/IRQ-/Warm-Sleep-/Recovery-Kapselung, aufgeschobene Funkparameteraenderungen |
| `src/helpers/radiolib/RadioLibWrappers.h/.cpp` | Kleine Hooks fuer Praeambel-vs-Paket-Interrupt, Metadaten und Zustands-/RX-Fehlerabfrage |
| `examples/simple_repeater/RepeaterPowerManager.h` (neu) | Uhrtrust, UTC-Fenster und zwei Wake-Grenzen |
| `examples/simple_repeater/MyMesh.h/.cpp` | Admin-CLI, Persistenzanbindung, Night-Pending-Pruefung und Power-Uebergaenge |
| `examples/simple_repeater/main.cpp` | RTC1/FreeRTOS-Timer, Nacht-Loop und lokale Erreichbarkeit |
| `src/helpers/CommonCLI.h/.cpp` | Power-Prefs und Hook nach akzeptierter Uhrsetzung; alte Prefs bleiben kompatibel |
| `create-uf2.py` | Korrekte Pfadquotierung und Fehlerrueckgabe |
| `tests/` | Ausfuehrbare C++-Policy-/API-Tests und Runner |

`CustomSX1262.h`, `CustomSX1262Wrapper.h`, `NRF52Board.cpp`, Routing, Keys und Neighbor-Zeitverhalten bleiben im offiziellen Quellstand. Die neuen Wrapper verwenden diese vorhandenen Implementierungen.
