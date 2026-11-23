<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport · ChatGPT Voice Companion

Based on the open-source [FoloToy AI Passport](https://github.com/folotoy/ai-passport) firmware, this project turns the
device into a pocket real-time voice assistant. It connects over Wi-Fi straight to the official OpenAI Live API offered
by [XAI Router](https://xairouter.com/blog/gpt-live-1-openai-and-codex/) and uses the full-duplex voice model
`gpt-live-1`: after power-on it joins the network and starts a conversation by itself; it is full duplex, so you can
talk over it while it speaks; five minutes without talking ends the conversation. During a conversation the screen shows
only the state orb and one status line.

## Using it

| Button | Action |
| --- | --- |
| OK short press | Start a conversation when idle; interrupt the assistant at once while it talks (or just start speaking); retry after an error |
| OK long press | End the conversation |
| ▲ long press | Open phone setup (change Wi-Fi / key) |
| ▲ / ▼ short press | Volume up / down by 10% (default 150%, the maximum; 0–100% is the codec volume, 110–150% adds 1.4–3x digital gain with a soft limiter, so it does not distort) |

The middle of the screen shows the animated ChatGPT logo:

| Logo | State |
| --- | --- |
| White, turning slowly and gently breathing | Ready, press OK to start (the title, Wi-Fi dot and battery are hidden during a conversation) |
| Grey, blinking | Joining Wi-Fi, checking the network or connecting to ChatGPT |
| White, swelling with your voice | Listening to you |
| White, spinning faster and pulsing with the reply | The assistant is answering |
| Red, still | Error, the status line shows why; press OK to retry |
| QR code | Setup mode: scan it with a phone to join the hotspot, then enter Wi-Fi and the API key |

"ChatGPT" and its logo are trademarks of OpenAI; this project is not affiliated with OpenAI.

Say a short goodbye such as "拜拜", "再见" or "关闭吧" ("bye", "goodbye", "turn off") and the conversation ends once the
assistant has said goodbye. Only a whole utterance of at most 12 characters counts, so a question like "how do I turn off
updates on my computer" does not end it, and talking on after the goodbye keeps the conversation going.

**Automatic power off**: after a conversation ends (or while offline), 10 minutes without talking or a button press shows
"10 分钟未使用，已自动关机" (unused for 10 minutes, powered off) and the device enters deep sleep (Wi-Fi disconnects
cleanly first). Press any button to switch it on; it goes straight into a conversation. It never powers off while a
computer is attached over USB (debugging and flashing); on a plain charger it powers off as usual.

Billing: `gpt-live-1` is billed by session time, silence included. Five minutes without any talking end the session, and
one session lasts at most 30 minutes. Turn off the automatic conversation at power-on with `autostart off`.

## First-time setup (scan with a phone, about a minute)

1. On first power-on (not configured yet) the screen shows a QR code and "扫码连接 ChatGPT-XXXX" (scan to join ChatGPT-XXXX).
2. Scan it with the phone camera to join the device hotspot; the setup page opens by itself (if not, open any web address
   or `192.168.4.1` in a browser).
3. Step 1: pick a nearby Wi-Fi network (2.4 GHz only) and enter its password (shown in plain text).
4. Step 2: paste your [XAI Router](https://xairouter.com) API key and tap "保存并重启" (Save and restart).
5. The device first checks that it can actually join that Wi-Fi before saving it, then starts a conversation. If it
   cannot connect, the QR code returns and the page explains why; the previous settings are kept.

To change Wi-Fi or the key later: **long-press ▲** to open setup again (leave the key empty to keep it); press OK to
leave setup. Full-width punctuation typed with a Chinese keyboard is converted to half-width and leading/trailing
spaces are removed automatically. The API host is fixed to `api.xairouter.com`.

Developers can also configure the device over USB serial (the key is never shown or written to a file):

```bash
. ~/esp/esp-idf-v5.5.3/export.sh          # provides pyserial
python3 tools/live_provision.py --ssid YourWiFiName
```

Other options: `--host`, `--voice` (default `marin`), `--prompt "assistant instructions"`, `--show` (current settings,
key masked). Flashing the merged `full.bin` at `0x0` erases the settings; to update only the program, write
`build/firmware/<hash>/FoloToy-AI-Passport.bin` to `0x10000` and the settings are kept.

## How it works

```text
mic ──► μ-law ──► uplink task ──► session.input_audio.append ──────────────────┐
                                                                              ▼
                                   wss://api.xairouter.com/v1/live/sessions (gpt-live-1)
                                                                              │
speaker ◄── jitter buffer (catches up) ◄── Base64 decode ◄── session.output_audio.delta ◄─┤
status line ◄──────────────────── session.input/output_transcript.delta ◄──────┘
```

- **Audio format**: 8 kHz G.711 μ-law (`audio/pcmu`) both ways, about 11 KB/s per direction and robust on weak links.
  gpt-live-1 does not allow different formats per direction; 16 kHz PCM does not fit the RAM of this PSRAM-less chip and
  was removed.
- **Network layer**: an own minimal WebSocket client (`live_ws.c`); one network task owns the TLS connection and does all
  sending and receiving, instead of `esp_websocket_client` (which holds a lock while receiving and starves the uplink when
  the link jitters). It reads only once a whole TLS record has arrived (mbedTLS dynamic buffers cannot handle half
  records) and enables `TCP_NODELAY`.
- **Uplink pacing**: gpt-live-1 paces its output by the input audio clock, so the uplink must stay strictly real-time.
  The microphone task only samples and queues; it never blocks.
- **TCP tuning**: the transoceanic path to US-East loses packets in the daytime. Without SACK (lwIP's default) the
  server's congestion window collapses and the downlink drops to 2–5 messages/s. `CONFIG_LWIP_TCP_SACK_OUT`, an 8-packet
  out-of-order queue and a 50 ms TCP timer are enabled.
- **Downlink buffer**: 600 ms of audio is buffered before playback; when jitter backs up more than 1.2 s, silent frames
  are skipped to catch up without cutting speech.
- **Full duplex**: the board has no acoustic echo cancellation. The microphone always streams and the model itself tells
  your interruptions from its own echo (the instructions mention it), so you can cut in just by talking. OK only clears
  local playback and briefly drops in-flight downlink audio so you can speak; no instruction is sent to the model.
- **Several Wi-Fi networks**: up to 4 saved networks; after power-on and after a disconnect it scans, joins the strongest
  usable one and moves on to the next on failure (for example home Wi-Fi, and a phone hotspot when away).
- **Network self-healing**: after joining and before each session, a TCP check against the API server runs; if it fails,
  the device disconnects cleanly and re-associates. During a session, 6 s without any downlink data counts as a dropped
  connection and the screen asks to reconnect instead of hanging silently.
- **No online lookups**: the device offers no lookup tools and the instructions ask the model to answer from its own
  knowledge. gpt-live-1 delegation cannot be switched off; if the model still sends `session.delegation.created`, the
  device replies at once "no online lookup here, please answer directly" so it does not wait forever.
- **Font**: the 16 px Chinese font contains only the characters the UI status text actually uses (extracted from the
  source by `tools/gen_live_font.py`, about 400 glyphs). Server error messages may contain other characters, which show as
  placeholder boxes.

## USB console commands

The device's USB serial console (`passport>`) supports these commands; `tools/live_provision.py` and
`tools/live_autotest.py` both work through it:

| Command | Action |
| --- | --- |
| `wifi <ssid> [password]`, `wifi_list`, `wifi_del <ssid>` | Add/update, list, delete saved Wi-Fi (up to 4; the strongest usable one is joined) |
| `key <api-key>`, `host`, `voice`, `prompt "<instructions>"` | Write settings (NVS) |
| `config`, `clear`, `restart`, `poweroff`, `setup` | Show settings (key masked), erase settings, restart, power off now, open phone setup |
| `debug on\|off` | Print session statistics and both transcripts to the log (off by default, not saved) |
| `talk`, `hangup` | Same as OK short press, OK long press |
| `autostart on\|off` | Start a conversation after power-on (on by default) |
| `nettest` | Wi-Fi signal, DNS and TCP connect time to the API server / Baidu |
| `testmic on\|off`, `testplay <url>`, `volume <0..150>` | Automated testing: replace the microphone with audio downloaded over the LAN, temporary volume |

Note: opening the USB serial port resets the chip.

## Automated test

```bash
. ~/esp/esp-idf-v5.5.3/export.sh
python3 tools/live_autotest.py --rounds 3 --nettest
```

The script opens the serial port once, waits for the device to come online and then runs each round: `talk` → a
question synthesized with macOS `say`, which the device downloads from a temporary local HTTP server and streams in real
time in place of the microphone (silent by default, device volume temporarily 0) → collect both transcripts and the
2-second statistics → `hangup`. Each round gets PASS/FAIL for: speech recognised, answer received, uplink/downlink rate,
underruns, lowest free heap and error logs. `--speaker` plays the question through the computer speaker instead;
`--barge` cuts in with a new question while the assistant tells a long story (OK by default, `--voice-barge` by voice
only); `--followup "one more"` asks a follow-up in the same session (add `--expect-close` to check that saying goodbye
ends it).

## Code layout

| Path | Contents |
| --- | --- |
| `main/main.c` | Start-up order and button mapping |
| `main/app_live.c` | Session state machine, network task (TLS + WebSocket), protocol events, drop detection, auto start/end |
| `main/app_audio.c` | Microphone and playback tasks, μ-law buffers, latency catch-up, clearing playback on OK, test microphone |
| `main/app_ui.c`, `main/chatgpt_logo.c` | Minimal UI (animated logo, one status line; title, Wi-Fi dot and battery while idle; setup QR code), logo bitmap |
| `main/app_wifi.c` | Wi-Fi, TCP check and clean re-association |
| `main/app_portal.c` | Phone setup: hotspot, captive DNS and the setup page |
| `main/app_power.c`, `main/app_diag.c` | Automatic power off (deep sleep), watchdog diagnostics |
| `main/app_config.c`, `main/app_console.c`, `main/app_testplay.c` | NVS settings, USB console commands, test audio download |
| `main/live_proto.c`, `main/live_ws.c`, `main/live_g711.c`, `main/live_policy.c`, `main/live_config.c`, `main/live_form.c` | Pure logic without ESP-IDF, covered by host tests |
| `tests/test_live_*.c` | Tests for protocol parsing, Base64, G.711, delegation events, the interrupt window, latency catch-up, volume, goodbye detection, settings and the setup page helpers |
| `tools/live_provision.py`, `tools/live_autotest.py` | USB provisioning script, end-to-end automated test |
| `tools/gen_live_font.py`, `assets/fonts/` | Chinese font generator, font and licence |

`main/demo_*.c` are the baseline hardware test pages, kept for their host tests and not built into this application.

## Build and validate

```bash
. ~/esp/esp-idf-v5.5.3/export.sh
./tools/validate.sh            # host tests + firmware build + merged image check
```

Output: `build/FoloToy-AI-Passport-full.bin`, flashed at `0x0`. Development notes are in [`AGENTS.md`](AGENTS.md) and the
[documentation index](docs/README.md).

## Known limitations and network notes

- No acoustic echo cancellation: full duplex relies on the model telling its own echo apart; at very high volume or in a
  very quiet room it may occasionally take the echo for an interruption. Lower the volume if that happens.
- The API key is stored in the device flash and can be read by whoever holds the device; use it only on your own device
  and limit or rotate the key in the XAI Router console if needed.
- Only XAI Router's official OpenAI Live API (`/v1/live/sessions`) is implemented. Codex Live V3 is WebRTC-only and would need
  an extra relay service.
- Some Wi-Fi 7 routers were seen to keep a broken client state after the device re-associates without a clean disconnect
  (for example after a reset): DHCP/DNS work but all TCP is dropped. The firmware's TCP check recovers by reconnecting
  cleanly; give the device a 2.4 GHz WPA2 SSID outside any MLO group.
- In ESP-IDF 5.5.3, linenoise in "dumb" terminal mode writes out of bounds when an input line reaches the command-line
  limit; never send overlong lines to the console (test audio therefore goes over LAN HTTP).
