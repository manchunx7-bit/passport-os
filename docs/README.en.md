[简体中文](../README.md) · **English**

# Passport OS

**A small device worth keeping in your daily routine.**

Community firmware for the FoloToy AI Passport, based on ESP32-C3 with 8 MB Flash. It brings thirteen apps together with shared settings, Wi-Fi setup, themes, audio, battery reporting, and power management.

**Ask Xiaozhi to adjust the volume, play music or radio, and set reminders on the device.**

## Why this project exists

Thank you to everyone who has enjoyed the project and shared feedback. I started Passport OS to make a small device I would use regularly, then selected thirteen functions and apps through repeated experiments.

One part I especially enjoy is letting Xiaozhi connect these functions. While chatting, you can ask to hear music, save a task, or start a focus session, and Xiaozhi calls the device's functions to carry out the request.

People have suggested many personal improvements. Because everyone uses the hardware differently, sharing the source lets you add what you need, remove what you do not, and make a version that fits you. Have fun using and modifying it.

## Ask Xiaozhi to operate the device

**Xiaozhi is the device's voice control entry point.** Double-press the OK button to enter a conversation, then use everyday language to adjust settings, open apps, play media, or manage tasks.

| Example request | Device action |
| --- | --- |
| “Turn the volume down” or “Set screen brightness to 50%” | Adjust and save the device's volume or brightness |
| “Play my liked songs and stop in half an hour” | Open NetEase Music, play your favorites, and set a stop timer |
| “Play traffic radio from Shenzhen” | Find a station by city and keywords, then open Radio to play it |
| “Start a Pomodoro” or “How much time is left?” | Start a 25-minute focus session or read the actual remaining time; pause and resume are also supported |
| “Remind me to drink water in ten minutes” or “Remind me to read every day at 9 pm” | Save a one-time or recurring reminder with an on-device alert and chime |
| “Create a packing list: keys, phone, umbrella” or “Mark umbrella as done” | Save a checklist, then read and update its items and completion state |
| “Open Stopwatch” or “Go to Home” | Open the requested app or screen |

You can also ask for today's AI news. Robot expressions and an audio waveform follow listening and speech, longer text expands into a reading area, and all four themes are supported.

Voice tools require a network connection and Xiaozhi configuration. NetEase playback also depends on your account and available tracks; radio depends on the online directory. Opening another app or starting media playback ends the current Xiaozhi conversation. Double-press OK again for further voice control. The device must remain powered on for reminders to trigger.

## Thirteen apps

| App | What it does | Requirements |
| --- | --- | --- |
| Home | Clock, profile, portraits, cards, weather, and air quality | Network for weather and air quality |
| Radio | City and station selection, playback, and sleep timer | Wi-Fi and available online stations |
| AI Voice | Hardware microphone and configurable desktop hotkeys | Windows companion program |
| Xiaozhi AI | Voice conversations, volume and brightness control, app opening, music and radio requests, timers, reminders, and lists | Network and Xiaozhi configuration; media needs its corresponding service |
| NetEase Music | Search songs, play playlists and daily recommendations | Wi-Fi, your own NetEase account, and available APIs |
| Reminders | One-time and recurring reminders, chimes, and snooze | Works offline; uses the device clock |
| Lists | View, check off, and manage tasks | Works offline |
| Muse | Send voice messages and read replies on screen | Phone pairing, your own account, and a working network |
| Why Barking | A sound-controlled game | Works offline |
| Sound Jump | Longer sounds jump farther; release your voice to jump | Works offline |
| Tetris | Stack blocks and clear lines | Works offline |
| Stopwatch | Timing, splits, pause, and resume | Works offline |
| Pomodoro | Focus sessions, breaks, and completion chimes | Works offline |

## Latest firmware

[Get the latest firmware](https://ai-passport.folotoy.cn/plays/438/?v=5579-4)

## Related projects

- [Muse: ai-passport-muse](https://github.com/manchunx7-bit/ai-passport-muse): standalone module source and usage guide.
- [AI Voice: folo-ai-passport-voice](https://github.com/manchunx7-bit/folo-ai-passport-voice): voice application source, Windows companion, and usage guide.

## Install

Using a prebuilt image does not require AI tools or a firmware development environment. Get it from the [latest firmware page](https://ai-passport.folotoy.cn/plays/438/?v=5579-4).

Use only the matching FoloToy AI Passport hardware revision with ESP32-C3 and 8 MB Flash. Similar-looking boards may have incompatible displays, audio chips, buttons, or partition layouts.

| Situation | Image | Address |
| --- | --- | --- |
| First installation or a fresh start | `*-full.bin` | `0x0` |
| Existing Passport OS with a verified identical partition layout | `*-app.bin` | `0x10000` |

**Back up first.** A full image overwrites configuration in its written range. A complete reset also requires erasing flash. To preserve settings and images, verify the partition layout and use the application image without erasing the whole flash.

On first boot, connect your phone to the device's password-free `Passport-WiFi-xxxx` hotspot and open `http://192.168.4.1`. Follow the setup page to select Wi-Fi, city, and personal details. Xiaozhi and Muse need your own service configuration; AI Voice also needs its Windows companion.

## Make your own version

Download the source, modify the apps or interface, build a firmware image, and test it on matching hardware.

| Area | Source |
| --- | --- |
| Application UI and logic | [`main/apps/`](../main/apps/) |
| Launcher, settings, and app lifecycle | [`main/launcher/`](../main/launcher/) |
| Xiaozhi device control and media handover | [`main/device_control_mcp.cc`](../main/device_control_mcp.cc), [`main/voice_intent.cc`](../main/voice_intent.cc) |
| Shared UI and fonts | [`main/ui/`](../main/ui/), [`main/fonts/`](../main/fonts/) |
| Board drivers | [`components/bsp/`](../components/bsp/) |
| Wi-Fi setup | [`components/esp-wifi-connect/`](../components/esp-wifi-connect/) |
| Muse integration | [`components/passport_muse/`](../components/passport_muse/) |
| Tests and development tools | [`tests/`](../tests/), [`tools/`](../tools/) |

### Build

Install Git, ESP-IDF 5.5.3, and its toolchain. On Windows, you can build in WSL and flash through a Windows serial port.

```bash
git clone https://github.com/manchunx7-bit/passport-os.git
cd passport-os
bash build.sh release
```

The script loads ESP-IDF from `/opt/esp-idf` by default. For another installation path:

```bash
IDF_DIR="$HOME/esp/esp-idf" bash build.sh release
```

ESP-IDF Component Manager downloads the declared dependencies. The build verifies distribution defaults, including public mode and empty proxy defaults.

- Full image: `build-public/FoloToy-AI-Passport-full.bin`, written at `0x0`.
- Application image: `build-public/passport-os.bin`, written at `0x10000` only for a verified matching partition layout.

### Verify

With ESP-IDF 5.5.3 activated:

```bash
bash tools/validate.sh --static
bash tools/validate.sh --firmware
```

The firmware validation command builds and verifies a full image at `build/FoloToy-AI-Passport-full.bin`. Build success does not replace physical testing of buttons, display, audio, networking, and the behavior you changed.

### Package

After validating the corresponding firmware version:

```bash
python3 tools/package_firmware.py --date YYYY-MM-DD --verification /path/to/verification-public.json
```

Replace the date and verification path with actual values. Use public-safe verification records for the exact version. Packaging does not publish a GitHub Release. Source changes require a new build and verification; do not reuse stale validation records.

## Privacy and licensing

Public builds enable `PASSPORT_PUBLIC_RELEASE=ON` and clear proxy defaults. Users supply their own Wi-Fi, accounts, and service credentials. Do not commit credentials, device exports, private logs, or personal images.

The main project uses the [MIT license](../LICENSE). Muse, Noise, and other third-party content retain their own licenses and notices. Jollybot artwork has [separate terms](../main/apps/muse/JOLLYBOT-NOTICE.md).

Thanks to FoloToy, the upstream projects, and everyone who shares ideas, reports issues, and contributes improvements.
