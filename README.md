# ESP32 Access Control and Room Lighting System

An ESP-IDF project for an ESP32-based entrance/exit access-control system and
two-room motion-activated lighting controller. The project is written in C
and uses FreeRTOS tasks and synchronization primitives. 
The hardware circuit can also be run in Wokwi.

## Features

- Two RC522 RFID readers, assigned to the entry and exit doors.
- Registered-user access checks with entry/exit anti-passback.
- Door schedule, stored in ESP32 NVS, with time supplied by a DS1307 RTC.
- Servo-operated doors with green access and red denial LEDs.
- Two PIR-controlled room lights, each with a manual hold button.
- Event logging to an SD card and a UART serial command interface.
- Administrator login for configuration commands.

Room PIR sensors detect motion and indicate occupied/empty status; they do not
count the number of people in a room.

## Hardware and pin assignments

| Device / signal | ESP32 GPIO |
|---|---:|
| SPI SCLK / MISO / MOSI (shared by RFID readers and SD card) | 18 / 19 / 23 |
| Entry RFID reader CS / exit RFID reader CS | 5 / 17 |
| Shared RFID reset | 4 |
| SD card CS | 16 |
| DS1307 RTC SDA / SCL | 21 / 22 |
| Entry / exit servo PWM | 25 / 26 |
| Entry green / red LEDs | 32 / 33 |
| Exit green / red LEDs | 27 / 14 |
| Room 1 PIR / relay / button | 34 / 12 / 2 |
| Room 2 PIR / relay / button | 35 / 13 / 15 |

The wiring diagram is in [`diagram.json`](diagram.json). GPIO assignments and
related timing constants are defined in [`main/app_config.h`](main/app_config.h).
GPIO 12 is a boot-strapping pin; the project configuration warns that attached
hardware on this pin may affect booting on a physical board.

## Requirements

- ESP-IDF with its tools installed and available in an ESP-IDF terminal.
- An ESP32 DevKit C V4-compatible board for physical use, or Wokwi for
  simulation.
- The project-managed RC522 dependency in `components/`.

The project has been configured for an ESP32 target. If configuring it for the
first time, run `idf.py set-target esp32` from the project directory.

## Build, flash, and monitor

Open an ESP-IDF terminal, change to this directory (the directory containing
the top-level `CMakeLists.txt`), then run:

```powershell
cd C:\path\to\switch\entrance_exit
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the serial port for the ESP32. To remove generated build
output and reconfigure from scratch, run `idf.py fullclean`, then run
`idf.py build`. The `build/` directory is generated and should not be edited
manually.

For Wokwi, open the project using [`wokwi.toml`](wokwi.toml) and
[`diagram.json`](diagram.json); the Wokwi configuration points to the firmware
and ELF files produced in `build/`.

## Serial console

The console uses UART0. After startup, enter `help` to display the command
list. Commands marked **admin-only** require a successful login.

| Command | Purpose |
|---|---|
| `help` | Show available commands. |
| `status` | Show clock, schedule, inside count, SD status, light timeout, and admin login status. |
| `inside` | List registered users currently inside. |
| `users` | List registered users and their inside/outside status. |
| `rooms` | Show room occupancy, light state, and light on-time. |
| `log [n]` | Print recent SD log records (defaults to 10; maximum 20). |
| `time` | Print the current system time. |
| `schedule` | View the door schedule. |
| `login <password>` / `logout` | Start or end an administrator session. |
| `schedule HH:MM HH:MM` **admin-only** | Change the schedule. |
| `settime YYYY-MM-DD HH:MM[:SS]` **admin-only** | Set the system time and, if available, the RTC. |
| `adduser <hex UID> <name>` **admin-only** | Register an RFID tag. |
| `deluser <hex UID>` **admin-only** | Remove a registered tag. |
| `lighttimeout <seconds>` **admin-only** | Set the room motion timeout (1–3600 seconds). |
| `passwd <new password>` **admin-only** | Change the admin password (minimum six characters). |
| `emergency` **admin-only** | Queue an open request for both doors. |
| `reboot` **admin-only** | Save user data and restart the ESP32. |

On first boot, the default administrator password is `admin123`; change it
with `passwd <new password>`. The administrator session expires after two
minutes without admin commands, and three failed login attempts trigger a
30-second lockout.

## Behavior and data storage

- Entry scans are denied for unknown tags and for users already marked inside.
- Exit scans are denied for unknown tags and for users not marked inside.
- An allowed entry/exit updates the user's inside/outside state. If the
  corresponding door queue cannot accept the request, that state change is
  reverted.
- On first boot, the demo user list contains Alice (`AA BB CC DD`) and Bob
  (`11 22 33 44`), unless user data is already present in NVS.
- When the RTC time is valid, the schedule is enforced for both entry and
  exit. If the system clock is invalid, schedule enforcement is bypassed until
  the time is set.
- Room motion keeps its light on until the configured timeout expires. A
  manual hold can keep the light on independently of motion.
- Users and settings are stored in ESP32 NVS. Event records are appended to
  `/sdcard/access.log` on the SD card. Log records contain timestamp, event
  type, UID, name, and detail.
- The SD mount configuration allows formatting if mounting fails. Use a
  disposable/simulated card for testing and keep a backup of any SD-card data
  you need.

## Project layout

```text
entrance_exit/
  CMakeLists.txt
  sdkconfig
  wokwi.toml
  diagram.json
  components/
    abobija__rc522/
  main/
    main.c
    app_config.h
    access.c / access.h
    cli.c / cli.h
    doors.c / doors.h
    event_log.c / event_log.h
    rfid.c / rfid.h
    rooms.c / rooms.h
    rtc_clock.c / rtc_clock.h
    settings.c / settings.h
    sha256.c / sha256.h
    util.c / util.h
```
