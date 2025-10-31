# Bomberman ESP-NOW (ESP32) — Project README

This repository contains a two-device local multiplayer Bomberman-like game for ESP32 devices using ESP-NOW for peer-to-peer communication. Two sketches are provided:

- `ESPNOW_LCDA/ESPNOW_LCDA.ino` — Player 1 (left display variant)
- `ESPNOW_LCDB/ESPNOW_LCDB.ino` — Player 2 (right display variant)

Both sketches rely on shared headers in each folder: `espnow_net.h`, `espnow_game.h`, `game_engine.h`, `debug.h`, and `menu.h`.

## Features

- Local two-player game using ESP-NOW (no Wi-Fi AP/router required).
- Deterministic map sync via runtime MAP_SYNC message (player 0 is authoritative for seed).
- Reliable bomb placement/ explosion messages with retransmit.
- Visual explosion cells with damage rules and respawn invulnerability.
- Minimal Serial logging: prints Local & Peer MAC on startup (other debug disabled by default).

## Recent important behavioral fix

Previously bomb placement messages transmitted the sender's absolute `millis()` timestamp. If the two ESP32 devices' clocks were not synchronized, receivers could interpret that timestamp as already past the fuse and trigger an immediate explosion. The code was updated to:

- Transmit the bomb "age" (milliseconds since placement) in the `placedMs` field of `MSG_BOMB_PLACE` instead of absolute sender timestamps.
- Receivers interpret `placedMs` as age and compute `placedAt = millis() - age` locally.

This removes the need for synchronized clocks and prevents immediate remote explosions due to clock skew.

## Hardware

Minimum hardware per player:

- ESP32 (any common development board should work).
- Two SPI/I2C OLED or SH110x displays used in the original project (128x128 displays supported via Adafruit_SH110X library).
- 5 input buttons wired to configured pins (default in sketches): Up=4, Down=5, Left=6, Right=7, Start/Bomb=15.

Wiring notes

- Displays in the sketches use two TwoWire objects; check the `I2C_1.begin(SDA, SCL)` and `I2C_2.begin(SDA, SCL)` lines in each sketch and adapt the pins if your board uses different pins.
- Buttons are wired with INPUT_PULLUP (button to GND). If you use external pull-downs or different wiring, adjust the `setupButtons()` or `btnPins` array.

## Files and responsibilities

- `ESPNOW_LCDA.ino` / `ESPNOW_LCDB.ino` — Game loop, UI, ESP-NOW initialization, player-specific configuration.
- `espnow_net.h` — ESPNOW transmit/receive glue and ping/pong helper used to check peer reachability.
- `espnow_game.h` — Game protocol packet definitions and send helpers (MSG_BOMB_PLACE, MSG_BOMB_EXPLODE, MSG_INPUT, etc.).
- `game_engine.h` — Map generation, bomb/ explosion handling, damage application hooks.
- `debug.h` — Macro-based debug helpers. When `ENABLE_DEBUG` is defined, DBG_* macros print to Serial. By default in this repo DBG_* are disabled and only MACs are printed via Serial.
- `menu.h`, `sprites.h` — Menu UI and sprite data.

## Build & Flash

You can use Arduino IDE, PlatformIO, or arduino-cli. Replace `YOUR_FQBN` and `COM_PORT` with your board definition and serial port.

Arduino IDE

1. Open the folder containing the sketch (e.g., `ESPNOW_LCDA/ESPNOW_LCDA.ino`) in Arduino IDE.
2. Select the proper ESP32 board (Tools > Board).
3. Select the correct port.
4. Upload.

arduino-cli (example)

Open `cmd.exe` (Windows) and run (replace placeholders):

```bat
arduino-cli compile -b <YOUR_FQBN> "h:\\Documents\\Arduino\\ESPNOW Project\\ESPNOW_LCDA"
arduino-cli upload -p COM3 -b <YOUR_FQBN> "h:\\Documents\\Arduino\\ESPNOW Project\\ESPNOW_LCDA"

arduino-cli compile -b <YOUR_FQBN> "h:\\Documents\\Arduino\\ESPNOW Project\\ESPNOW_LCDB"
arduino-cli upload -p COM4 -b <YOUR_FQBN> "h:\\Documents\\Arduino\\ESPNOW Project\\ESPNOW_LCDB"
```

PlatformIO

- Create a new PlatformIO project for your ESP32 board and add the folders as separate examples or projects. Use the usual `pio run` / `pio run -t upload` commands.

## Configuration before flashing

- Set peer MAC addresses in each sketch `peer_mac[]` with the other device's MAC address. You can either hardcode it (as in the sketches) or implement a simple config UI. The sketches print `Local MAC` on Serial at startup so you can copy/paste it to the peer.
- `myPlayerId` is set to `0` for LCDA and `1` for LCDB by default. Change only if you swap roles.

## Runtime / Testing steps

1. Flash LCDA to one ESP32, LCDB to the other.
2. Open Serial Monitor at 115200 for both devices; check startup output:
   - Local MAC: AA:BB:CC:DD:EE:FF
   - Peer MAC:  XX:XX:XX:XX:XX:XX
3. On both devices open the menu, press Start to enter the waiting page, and then start the game on both devices.
4. Place bombs and ensure both devices show the bomb and explode approximately at the same time.

Test cases

- Normal: Both devices in menu → start → place bombs — verify explosions synchronized.
- Delayed join: Place a bomb on device A, then power-cycle device B and let it rejoin — the code uses an "age" in bomb place; if B receives the placement it should compute remaining fuse correctly or treat it as stale if too old.
- Packet loss: Retransmit interval `BOMB_PLACE_RESEND_MS` is 250ms while the bomb is active; this helps peers receive placements reliably.

## Debugging & Logs

- By default, general debug macros (`DBG_PRINT`, `DBG_PRINTF`, etc.) are disabled to reduce Serial spam. The sketches still initialize Serial and print only: Local MAC and Peer MAC.
- To re-enable full debug output, define `ENABLE_DEBUG` at the top of the sketch or in `debug.h`. Example: add `#define ENABLE_DEBUG` near the top of `ESPNOW_LCDA.ino` and `ESPNOW_LCDB.ino` before including `debug.h` or modify `debug.h` itself.

Important logs to inspect when troubleshooting bomb timing:

- `RX BOMB PLACE id=... age=... fuse=...` — shows the received age and fuse, useful to check whether the receiver thinks a bomb is already expired.
- `RX BOMB PLACE (slightly expired, scheduling)` — the code scheduled a near-immediate explosion because age >= fuse but still within `BOMB_STALE_THRESHOLD_MS`.
- `RX BOMB PLACE (stale)` — the placement is too old and was treated as already exploded.

If you see immediate explosions on the receiving side, attach Serial logs for these messages and check the `age` values printed.

## Protocol notes (summary)

- MSG_BOMB_PLACE fields (packed): header, bombId (u16), x (u8), y (u8), placedMs (u32), fuseMs (u16)
  - Important: `placedMs` now contains "age" (ms since placement) rather than absolute sender millis().
- MSG_BOMB_EXPLODE: header, bombId, cx, cy, explodeMs (u32) — used for explicit explode notifications.

## Troubleshooting

- If the peer MAC isn't set correctly the devices won't communicate. Use the printed `Local MAC` from one device and paste it into the other's `peer_mac[]` array.
- If bombs still explode immediately on one side:
  - Verify the `age` printed in `RX BOMB PLACE` logs; if it's >= fuse and far beyond `BOMB_STALE_THRESHOLD_MS`, the placement really is stale.
  - Check that retransmits are being sent (look for repeated `send_bomb_place` calls in code or enable DBG to print send events).
- If displays or I2C fail, verify the SDA/SCL pins set in `I2C_1.begin()` and `I2C_2.begin()` match your wiring.

## Potential improvements and next steps

- Make MAC printing optional via a compile-time flag (e.g., `PRINT_MACS`) instead of hardcoding Serial calls.
- Add explicit version or build tag printed at startup.
- Implement an in-game reconnection or peer discovery UI to avoid manual MAC config.
- Add unit tests / simulation harness to test bomb timing logic under simulated clock skews.

## License

This project is provided as-is. Add your preferred license (MIT, Apache-2.0, etc.) by creating a `LICENSE` file.

---
