//-----------------------------------------------------------------------------
// Bomberman ESP32 - LCDB (Player 2)
//-----------------------------------------------------------------------------
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "sprites.h"
#include "espnow_net.h"
#include "espnow_game.h"
#include "menu.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include "debug.h"

// disable general debug prints; we'll only initialize Serial to print MAC addresses
//-----------------------------------------------------------------------------
// Hardware Configuration
//-----------------------------------------------------------------------------
// I2C Display Configuration
TwoWire I2C_1 = TwoWire(0);
TwoWire I2C_2 = TwoWire(1);
Adafruit_SH1107 display1(128, 128, &I2C_1);
Adafruit_SH1107 display2(128, 128, &I2C_2);

// Display throttling to reduce I2C blocking during gameplay
const unsigned long DISPLAY_REFRESH_MS = 33; // ~30 FPS
unsigned long lastDisplay1FlushMs = 0;
unsigned long lastDisplay2FlushMs = 0;

void flushDisplay1(bool force = false) {
  unsigned long now = millis();
  if (force || now - lastDisplay1FlushMs >= DISPLAY_REFRESH_MS) {
    display1.display();
    lastDisplay1FlushMs = now;
  }
}

void flushDisplay2(bool force = false) {
  unsigned long now = millis();
  if (force || now - lastDisplay2FlushMs >= DISPLAY_REFRESH_MS) {
    display2.display();
    lastDisplay2FlushMs = now;
  }
}

// Timing / counters
unsigned long lastUpdate1 = 0;
unsigned long lastUpdate2 = 0;
uint32_t counter = 0;

// Tile/map configuration: use 8x8 tiles and a 16x16 arena so the full map fits
// exactly into a 128x128 display (16 * 8 = 128). HUD is shown on the
// second display, so reserve no HUD height on the gameplay display.
const uint8_t TILE_SIZE = 8;
const uint8_t MAP_COLS = 16; // reduce from 32 -> 16 so full arena fits horizontally
const uint8_t HUD_HEIGHT = 0; // gameplay shown full-screen on display1
const uint8_t MAP_ROWS = 16;  // make square 16x16 map to use full 128px height

enum Tile : uint8_t { TILE_EMPTY = 0, TILE_SOLID = 1, TILE_BREAKABLE = 2 };

// Game state (storage)
#include "game_engine.h"

Tile mapData[MAP_ROWS][MAP_COLS];
int playerX = 1, playerY = 1, playerHealth = 1;
int otherPlayerX = -1, otherPlayerY = -1; bool otherPlayerVisible = false;
int spawnX = 1, spawnY = 1;
const int MAX_BOMBS = 6;
const int MAX_EXPLOSION_CELLS = 128;

// concrete storage for bombs/explosions (types Bomb/ExplosionCell are defined in game_engine.h)
Bomb bombs[MAX_BOMBS];
ExplosionCell explosions[MAX_EXPLOSION_CELLS];

// Gameplay parameters
const unsigned long BOMB_FUSE = 2000;
const unsigned long EXPLOSION_VIS_MS = 300;
const int EXPLOSION_RADIUS = 2;

// Reliable placement helpers: retransmit interval and stale thresholds
const unsigned long BOMB_PLACE_RESEND_MS = 250; // resend bomb_place while bomb active
const unsigned long BOMB_MIN_REMAIN_MS = 150;   // when a remote place is slightly expired, leave a small remainder
const unsigned long BOMB_STALE_THRESHOLD_MS = 1000; // if placement is older than this, treat as exploded
unsigned long lastBombPlaceSent[MAX_BOMBS] = {0};

// HUD / scoring
int lives = 3;
long score_local = 0;
long score_remote = 0;
// legacy global used by game_engine.h fallback paths
long score = 0;

// Invulnerability timings
const unsigned long SPAWN_INVUL_MS = 3000; unsigned long spawnInvulEnd = 0;
const unsigned long PLAYER_INVUL_MS = 800; unsigned long lastPlayerHitAt = 0;
int explosionEventCounter = 0; int lastDamageEvent = 0;

// --- networking: set the other ESP32's MAC here (update to your opponent)
uint8_t peer_mac[] = {0x98, 0xA3, 0x16, 0xEB, 0xCE, 0x3C}; // TODO: change to other device MAC

// Game state
bool gameOver = false;
// final winner id when game ends (-1 = unknown)
int finalWinnerId = -1;

// Debug flag: set to true to print hit/ explosion diagnostics over Serial
const bool DEBUG_HITS = true;

//-----------------------------------------------------------------------------
// Input Configuration
//-----------------------------------------------------------------------------
// Button pin assignments (4-7: movement, 15: Start/Bomb)
const int BTN_UP_PIN = 4;
const int BTN_DOWN_PIN = 5;
const int BTN_LEFT_PIN = 6;
const int BTN_RIGHT_PIN = 7;
const int BTN_BOMB_PIN = 15; // combined Start/Bomb

// Input timing (tuned for low latency while maintaining debounce)
const unsigned long DEBOUNCE_MS = 12;
const unsigned long POLL_MS = 10;
// Movement repeat when holding a direction (ms between repeated moves)
const unsigned long MOVE_REPEAT_MS = 150;

// mapping: bit0=UP, bit1=DOWN, bit2=LEFT, bit3=RIGHT, bit4=BOMB
uint8_t lastStableFlags = 0;
unsigned long lastPollMs = 0;
unsigned long lastChangeTimeBtns[5] = {0,0,0,0,0};
int btnPins[5] = {BTN_UP_PIN, BTN_DOWN_PIN, BTN_LEFT_PIN, BTN_RIGHT_PIN, BTN_BOMB_PIN};
bool lastRawBtn[5] = {false,false,false,false,false};

// your player id (set when decided/assigned)
// Default to 1 on this device so LCDB acts as player 2 (bottom-right)
uint8_t myPlayerId = 1;

// Game/Menu state
enum GameState : uint8_t { STATE_MENU = 0, STATE_WAITING = 1, STATE_GAME = 2, STATE_ENDING = 3 };
GameState gameState = STATE_MENU; // start in menu by default

// Waiting/sync variables
bool localReady = false;
bool peerReady = false;
bool countdownStarted = false;
int countdownSec = 3;
unsigned long lastCountdownUpdate = 0;
unsigned long waitingStartedAt = 0;
const unsigned long WAIT_FOR_PEER_MS = 7000; // ms to wait for peer before starting solo
uint8_t waitingLastBtnFlags = 0;
unsigned long peerReadyAt = 0;

void enterGame() {
  gameState = STATE_GAME;
  DBG_PRINTLN("STATE: enter GAME");
  // set spawn invulnerability early so any incoming explosion callbacks
  // during initialization won't damage the player
  spawnInvulEnd = millis() + SPAWN_INVUL_MS;
  // re-init game state when entering game
  initializeGame();
  // Position player according to assigned player id.
  // Convention: myPlayerId == 0 -> Player 1 (top-left). Any other id -> Player 2 (bottom-right).
  // store spawn coordinates so respawn returns to this location
  if (myPlayerId == 0) {
    spawnX = 1; spawnY = 1;
  } else {
    spawnX = MAP_COLS - 2; spawnY = MAP_ROWS - 2;
  }
  playerX = spawnX; playerY = spawnY;
  // spawnInvulEnd already set before initializeGame
  // Announce ourselves to peer: send JOIN and current position so peer can show us immediately
  send_join(myPlayerId);
  send_pos(myPlayerId, (uint8_t)playerX, (uint8_t)playerY);
}

void enterMenu() {
  gameState = STATE_MENU;
  DBG_PRINTLN("STATE: enter MENU");
  // optional: show the startup menu if implemented
  // showStartupMenu(display1, display2);
  // Use centralized menu implementation from menu.h
  showStartupMenu(display1, display2);
  // Ensure menu buttons are configured (we use combined START/BOMB pin)
  setMenuButtonPins(BTN_BOMB_PIN, -1, -1);
  configureMenuButtons();
  // reset waiting/sync state so entering menu clears any previous readiness
  localReady = false;
  peerReady = false;
  peerReadyAt = 0;
  countdownStarted = false;
  waitingStartedAt = 0;
  // Don't block here. The menu is now interactive and non-blocking.
  // Background peer checks and Start/Settings selection are handled in pollButtonsAndSend(menuActive=true).
}

void setupButtons() {
  for (int i = 0; i < 5; ++i) pinMode(btnPins[i], INPUT_PULLUP); // button -> GND
}

//-----------------------------------------------------------------------------
// Input & Event Handlers
//-----------------------------------------------------------------------------
uint8_t sampleButtonsDebounced() {
  uint8_t rawFlags = 0;
  unsigned long now = millis();
  bool raw[5];
  for (int i = 0; i < 5; ++i) {
    raw[i] = (digitalRead(btnPins[i]) == LOW); // pressed = LOW
    if (raw[i] != lastRawBtn[i]) {
      lastChangeTimeBtns[i] = now;
      lastRawBtn[i] = raw[i];
    }
  }
  for (int i = 0; i < 5; ++i) {
    if (now - lastChangeTimeBtns[i] >= DEBOUNCE_MS) {
      if (raw[i]) rawFlags |= (1 << i);
    } else {
      if (lastStableFlags & (1 << i)) rawFlags |= (1 << i);
    }
  }
  lastStableFlags = rawFlags & 0x1F;
  return lastStableFlags;
}

void pollButtonsAndSend(bool menuActive=false) {
  if (millis() - lastPollMs < POLL_MS) return;
  lastPollMs = millis();
  uint8_t flags = sampleButtonsDebounced();
  if (menuActive) {
    // Menu selection: Up/Down to move, Start to choose.
    static int menuSel = 0; // 0 = Start, 1 = Settings
    static uint8_t lastMenuFlags = 0;
    static unsigned long lastMenuCheck = 0;
    bool upPressed = (flags & 0x01) != 0;
    bool downPressed = (flags & 0x02) != 0;
    bool startPressed = (flags & 0x10) != 0;

    // Edge detection for Up/Down
    if (upPressed && !(lastMenuFlags & 0x01)) {
      menuSel = max(0, menuSel - 1);
    }
    if (downPressed && !(lastMenuFlags & 0x02)) {
      menuSel = min(1, menuSel + 1);
    }

    // redraw left display to indicate selection
    display1.clearDisplay();
    display1.setTextSize(2);
    display1.setTextColor(1);
    display1.setCursor(16, 8);
    display1.print("MAIN MENU");
    display1.drawRect(8, 36, 112, 72, 1);
    display1.setTextSize(1);
    display1.setCursor(20, 48);
    if (menuSel == 0) display1.print("> Start"); else display1.print("  Start");
    display1.setCursor(20, 64);
    if (menuSel == 1) display1.print("> Settings"); else display1.print("  Settings");
    flushDisplay1(true);

    // periodic background connection check and update right display
    if (millis() - lastMenuCheck > 700) {
  lastMenuCheck = millis();
  bool connected = isPlayer2Connected();
      display2.clearDisplay();
      display2.setTextSize(1);
      display2.setCursor(4, 8);
      display2.print("Status:");
      display2.setCursor(4, 20);
      if (connected) display2.print("Player 1: Connected   ");
      else display2.print("Connecting to peers...");
      flushDisplay2(true);
    }

    // Start pressed -> take action for selection
    if (startPressed && !(lastMenuFlags & 0x10)) {
      if (menuSel == 0) {
        DBG_PRINTLN("MENU: Start selected -> waiting for peer");
        enterWaiting();
      } else {
        // Settings placeholder
        DBG_PRINTLN("MENU: Settings selected (placeholder)");
        // show a simple placeholder screen
        display1.clearDisplay(); display1.setTextSize(1); display1.setCursor(8, 40); display1.print("Settings (TODO)"); flushDisplay1(true);
      }
    }

    lastMenuFlags = flags;
    return;
  }
  // If we're in ENDING, START returns to menu independently.
  if (gameState == STATE_ENDING) {
    static uint8_t lastEndFlags = 0;
    uint8_t anyNow = flags & 0x1F;
    uint8_t newPress = anyNow & (~lastEndFlags);
    if (newPress) {
      DBG_PRINTLN("ENDING: button pressed -> returning to menu");
      finalWinnerId = -1;
      gameOver = false;
      enterMenu();
    }
    lastEndFlags = anyNow;
    return;
  }
  static uint8_t lastSentFlags = 0;
  static unsigned long lastMoveAt = 0;
  uint8_t inputFlags = flags & 0x1F;
  unsigned long now = millis();
  bool shouldSend = false;

  // Immediate response to changes (edge) ------------------------------------------------
  if (inputFlags != lastSentFlags) {
    int nx = playerX;
    int ny = playerY;
    if (inputFlags & 0x01) ny--;
    if (inputFlags & 0x02) ny++;
    if (inputFlags & 0x04) nx--;
    if (inputFlags & 0x08) nx++;
    if (nx >= 0 && nx < MAP_COLS && ny >= 0 && ny < MAP_ROWS) {
      if (mapData[ny][nx] == TILE_EMPTY) { playerX = nx; playerY = ny; }
    }
    if ((inputFlags & 0x10) && !(lastSentFlags & 0x10)) {
      placeBombAtPlayer();
      for (int i = 0; i < MAX_BOMBS; i++) if (bombs[i].active && bombs[i].placedAt + 5 > now) {
        // send elapsed (age) instead of absolute millis() so peer can
        // compute remaining fuse using its own clock.
        uint32_t age = (uint32_t)(now - bombs[i].placedAt);
        send_bomb_place(myPlayerId, i, bombs[i].x, bombs[i].y, age, bombs[i].fuseMs);
        lastBombPlaceSent[i] = millis();
        break;
      }
    }
    shouldSend = true;
    lastMoveAt = now;
  } else {
    if ((inputFlags & 0x0F) != 0 && now - lastMoveAt >= MOVE_REPEAT_MS) {
      int nx = playerX;
      int ny = playerY;
      if (inputFlags & 0x01) ny--;
      if (inputFlags & 0x02) ny++;
      if (inputFlags & 0x04) nx--;
      if (inputFlags & 0x08) nx++;
      if (nx >= 0 && nx < MAP_COLS && ny >= 0 && ny < MAP_ROWS) {
        if (mapData[ny][nx] == TILE_EMPTY) { playerX = nx; playerY = ny; }
      }
      shouldSend = true;
      lastMoveAt = now;
    }
  }

  if (shouldSend) {
    send_input(myPlayerId, millis(), inputFlags);
    send_pos(myPlayerId, (uint8_t)playerX, (uint8_t)playerY);
    lastSentFlags = inputFlags;
  }
}


// Called by game_engine when an explosion cell is created on (x,y).
// This centralizes damage rules. It intentionally ignores bombs present
// on the tile so standing on your own bomb does NOT grant immunity.
void damagePlayerAt(int x, int y, uint8_t ownerId, bool forceDamage, int eventId) {
  unsigned long now = millis();
  if (DEBUG_HITS) {
    DBG_PRINT("DEBUG: damagePlayerAt called force="); DBG_PRINT(forceDamage ? "yes" : "no");
    DBG_PRINT(" now="); DBG_PRINT(now);
    DBG_PRINT(" spawnInvulEnd="); DBG_PRINTLN(spawnInvulEnd);
  }
  // Respect spawn invulnerability for 3 seconds after spawn/respawn.
  // Even forced center explosions should NOT bypass spawn invulnerability.
  if (now < spawnInvulEnd) {
    if (DEBUG_HITS) DBG_PRINTLN("DEBUG: skipped due to spawn invulnerability");
    return;
  }
  // If this player was already damaged by the same explosion event, skip
  if (eventId != 0 && eventId == lastDamageEvent) {
    if (DEBUG_HITS) DBG_PRINTLN("DEBUG: already damaged by this event, skipping");
    return;
  }
  if (DEBUG_HITS) {
    DBG_PRINT("DEBUG: explosion at "); DBG_PRINT(x); DBG_PRINT(','); DBG_PRINT(y);
    DBG_PRINT(" player at "); DBG_PRINT(playerX); DBG_PRINT(','); DBG_PRINTLN(playerY);
    // check if a bomb is present on that tile
    bool bombOnTile = false;
    for (int i = 0; i < MAX_BOMBS; i++) if (bombs[i].active && bombs[i].x == x && bombs[i].y == y) bombOnTile = true;
    DBG_PRINT("DEBUG: bombOnTile="); DBG_PRINTLN(bombOnTile ? "yes" : "no");
  }
  if (playerX == x && playerY == y) {
    // One explosion hit removes one heart (life). Treat lives as hearts.
    if (DEBUG_HITS) DBG_PRINTLN("DEBUG: player TAKEN DAMAGE (life -1)");
    // record that we were damaged by this explosion event so we don't get hit again
    if (eventId != 0) lastDamageEvent = eventId;
  if (lives > 0) lives--;
    // respawn if still has lives, otherwise game over
    if (lives > 0) {
      playerHealth = 1; // 1 HP per life
      playerX = spawnX; playerY = spawnY;
      spawnInvulEnd = millis() + SPAWN_INVUL_MS;
    } else {
      // local player has no lives left -> apply death scoring and announce end
      // ownerId is the killer; credit killer +20 and penalize victim -20 locally
      if (ownerId != (uint8_t)0xFF) {
        // compute authoritative scores for player0 and player1
        int32_t s0 = 0, s1 = 0;
        if (myPlayerId == 0) { s0 = score_local; s1 = score_remote; }
        else { s0 = score_remote; s1 = score_local; }
        // apply killer +20
        if (ownerId == 0) s0 += 20; else s1 += 20;
        // apply victim -20 (victim is local player)
        if (myPlayerId == 0) s0 -= 20; else s1 -= 20;
        // update local mirrored scores
        if (myPlayerId == 0) { score_local = s0; score_remote = s1; }
        else { score_local = s1; score_remote = s0; }
        DBG_PRINTF("PLAYER DIED locally: victim=%u killer=%u (scores now s0=%ld s1=%ld)\n", myPlayerId, ownerId, (long)s0, (long)s1);
        // send authoritative snapshot to peer
        send_player_death((uint8_t)myPlayerId, ownerId, s0, s1, myPlayerId);
      }
      int winnerId = (myPlayerId == 0) ? 1 : 0;
      uint8_t payload[2];
      payload[0] = 0x01; // GAME_END code
      payload[1] = (uint8_t)winnerId;
      send_state_snapshot(payload, sizeof(payload), myPlayerId);
      finalWinnerId = winnerId;
      gameOver = true;
      gameState = STATE_ENDING;
    }
  }
}

// State snapshot from peer (used to announce game end)
void game_on_state_snapshot(const uint8_t *src_mac, const uint8_t *data, int len) {
  (void)src_mac;
  if (!data || len < 2) return;
  uint8_t code = data[0];
  if (code == 0x01) {
    uint8_t winnerId = data[1];
    finalWinnerId = (int)winnerId;
    gameOver = true;
    gameState = STATE_ENDING;
    DBG_PRINT("RX STATE SNAPSHOT: winner="); DBG_PRINTLN(finalWinnerId);
  }
  // MAP sync: payload = [0x02][4 bytes seed LE]
  else if (code == 0x02 && len >= 5) {
    unsigned long seed = 0;
    memcpy(&seed, data + 1, 4);
    pending_map_seed = seed;
    DBG_PRINT("RX MAP_SYNC seed="); DBG_PRINTLN(seed);
  }
}

//-----------------------------------------------------------------------------
// Display & UI Functions
//-----------------------------------------------------------------------------
void showGameOver() {
  // Determine local result text
  String resultMain = "GAME";
  String resultSub = "OVER";
  if (finalWinnerId >= 0) {
    if (finalWinnerId == myPlayerId) {
      resultMain = "YOU"; resultSub = "WIN";
    } else {
      resultMain = "YOU"; resultSub = "LOSE";
    }
  }

  // Draw left display: large centered title + framed score block
  display1.clearDisplay();
  display1.setTextSize(3);
  display1.setTextColor(1);
  int aW = resultMain.length() * 6 * 3;
  int bW = resultSub.length() * 6 * 3;
  int ax = max(0, (128 - aW) / 2);
  int bx = max(0, (128 - bW) / 2);
  // move title up 7px to improve layout on 128px displays
  display1.setCursor(ax, 11); display1.print(resultMain);
  display1.setCursor(bx, 35); display1.print(resultSub);
  display1.setTextSize(1);
  int boxX = 8, boxY = 79, boxW = 112, boxH = 34; // boxY moved up 7px
  display1.drawRoundRect(boxX, boxY, boxW, boxH, 4, 1);
  display1.setCursor(boxX + 10, boxY + 8); display1.print("YOU:"); display1.print(score_local);
  display1.setCursor(boxX + 10, boxY + 20); display1.print("THEM:"); display1.print(score_remote);
  // Place the footer just below the score box so it doesn't get clipped.
  display1.setCursor(8, 113); display1.setTextSize(1); display1.print("Press any button");
  flushDisplay1(true);

  // Draw right display (same look)
  display2.clearDisplay();
  display2.setTextSize(3);
  display2.setTextColor(1);
  display2.setCursor(ax, 11); display2.print(resultMain);
  display2.setCursor(bx, 35); display2.print(resultSub);
  display2.setTextSize(1);
  display2.drawRoundRect(boxX, boxY, boxW, boxH, 4, 1);
  display2.setCursor(boxX + 10, boxY + 8); display2.print("YOU:"); display2.print(score_local);
  display2.setCursor(boxX + 10, boxY + 20); display2.print("THEM:"); display2.print(score_remote);
  display2.setCursor(8, 113); display2.setTextSize(1); display2.print("Press any button");
  flushDisplay2(true);
}
// Draw prompt telling player how to return to menu after game end
void drawReturnToMenuPrompt() {
  // Footer is already rendered by showGameOver(); avoid redraw here to prevent
  // flicker/clipping caused by multiple overlapping draws.
  // Intentionally left empty.
}

//-----------------------------------------------------------------------------
// Map Generation Configuration
//-----------------------------------------------------------------------------
// Map seed behavior (0: randomize each game, non-zero: fixed deterministic map)
const unsigned long MAP_SEED = 0UL; // 0 => enable AUTO_RANDOMIZE_ON_START behavior
const bool AUTO_RANDOMIZE_ON_START = true;

// Pending runtime map seed received from peer or generated locally during countdown.
// If non-zero, initializeGame() will use this seed to deterministically generate the map.
unsigned long pending_map_seed = 0;

//-----------------------------------------------------------------------------
// Drawing Utilities
//-----------------------------------------------------------------------------
void drawFillRectU8(Adafruit_SH1107 &disp, int x, int y, int w, int h) { disp.fillRect(x, y, w, h, 1); }

// generateMap() provided by game_engine.h

// Tile drawing and game helpers are provided by game_engine.h

// Render a 128×128 window of the tilemap to the provided display.
// xPixelOffset = left column (in pixels) of the full map that maps to display x=0.
void renderMapToDisplay(Adafruit_SH1107 &disp, int xPixelOffset) {
  int leftTile = xPixelOffset / TILE_SIZE;
  int xWithin = xPixelOffset % TILE_SIZE;
  int tilesWide = 128 / TILE_SIZE + 1; // include partial tile

  for (int ry = 0; ry < MAP_ROWS; ry++) {
    for (int tx = 0; tx < tilesWide; tx++) {
      int mapCol = leftTile + tx;
      if (mapCol < 0 || mapCol >= MAP_COLS) continue;
      int px = tx * TILE_SIZE - xWithin;
      int py = ry * TILE_SIZE + HUD_HEIGHT;
      drawTile(disp, px, py, mapData[ry][mapCol]);
    }
  }

  // Draw player sprite if visible in this window
  int playerPixelX = playerX * TILE_SIZE;
  if (playerPixelX >= xPixelOffset && playerPixelX < xPixelOffset + 128) {
    int px = playerPixelX - xPixelOffset;
    int py = playerY * TILE_SIZE + HUD_HEIGHT;
    const int pw = 6, ph = 6;
    int pxoff = px + (TILE_SIZE - pw) / 2;
    int pyoff = py + (TILE_SIZE - ph) / 2;
    // If spawn invulnerability active, flash the player sprite (toggle every 200ms)
    unsigned long now = millis();
    bool spawnInvul = (now < spawnInvulEnd);
    if (!spawnInvul || ((now / 200) % 2 == 0)) {
      disp.drawBitmap(pxoff, pyoff, SPRITE_PLAYER_6x6, pw, ph, 1);
    }
  }
  // Draw remote player (if known)
  if (otherPlayerVisible) {
    int otherPixelX = otherPlayerX * TILE_SIZE;
    if (otherPixelX >= xPixelOffset && otherPixelX < xPixelOffset + 128) {
      int px = otherPixelX - xPixelOffset;
      int py = otherPlayerY * TILE_SIZE + HUD_HEIGHT;
      const int pw = 6, ph = 6;
      int pxoff = px + (TILE_SIZE - pw) / 2;
      int pyoff = py + (TILE_SIZE - ph) / 2;
      disp.drawBitmap(pxoff, pyoff, SPRITE_PLAYER_6x6, pw, ph, 1);
    }
  }
}

// HUD rendering - left display (Lives & Title)
void drawHUDLeft(Adafruit_SH1107 &disp) {
  disp.setTextSize(1);
  disp.setTextColor(1);
  // Show lives on top-left as small boxes (no text label to save space)
  int startX = 4;
  int startY = 6;
  for (int i = 0; i < lives; i++) {
    int lx = startX + i * 12;
    disp.drawBitmap(lx, startY, SPRITE_LIFE_8x6, 8, 6, 1);
  }
  // spawn invulnerability indicator (blinks while active)
  unsigned long now = millis();
  if (now < spawnInvulEnd) {
    if ((now / 300) % 2 == 0) {
      disp.fillRect(4 + lives * 12, startY, 8, 6, 1);
    }
  }
  // Title on the top-right edge
  // Disable wrapping and compute title X using actual character count so it doesn't wrap
  disp.setTextWrap(false);
  const char *title = "BOMBERMAN";
  int titleLen = 9; // length of "BOMBERMAN"
  int charWidth = 6; // approx pixel width per char at textSize=1
  int titleWidth = titleLen * charWidth;
  int titleX = 128 - 2 - titleWidth; // small right margin
  disp.setCursor(titleX, 2);
  disp.print(title);
  disp.setTextWrap(true);
}

// HUD rendering - right display (Score & Bombs)
void drawHUDRight(Adafruit_SH1107 &disp) {
  // Dedicated HUD for second display: Title + Hearts + YOU/THEM scores
  disp.setTextSize(2);
  disp.setTextColor(1);
  const char *title = "BOMBERMAN";
  int titleLen = strlen(title);
  int titleW = titleLen * 6 * 2; // approx width at textSize=2
  int titleX = max(0, (128 - titleW) / 2);
  disp.setCursor(titleX, 4);
  disp.print(title);

  // Draw lives/heart icons below the title
  int heartY = 28;
  for (int i = 0; i < lives; i++) {
    int hx = 12 + i * 16;
    disp.drawBitmap(hx, heartY, SPRITE_LIFE_8x6, 8, 6, 1);
  }

  // Scores
  disp.setTextSize(1);
  disp.setCursor(8, 56);
  disp.print("YOU: "); disp.print(score_local);
  disp.setCursor(8, 72);
  disp.print("THEM:"); disp.print(score_remote);

  // Bombs available small indicator at top-right
  int freeBombs = 0;
  for (int i = 0; i < MAX_BOMBS; i++) if (!bombs[i].active) freeBombs++;
  disp.setCursor(90, 56);
  disp.print("B:"); disp.print(freeBombs);
}

void setup() {
  // Initialize TwoWire buses for the two displays (pins chosen earlier)
  I2C_1.begin(17, 18, 100000); // SDA=18, SCL=17
  I2C_2.begin(9, 10, 100000);  // SDA=10, SCL=9

  // Initialize displays
  display1.begin(0x3C); // typical SH110x address; adjust if different
  display2.begin(0x3C);
  display1.clearDisplay();
  display2.clearDisplay();

  // Initial messages
  display1.setTextSize(1);
  display1.setTextColor(1);
  display1.setCursor(0, 10);
  display1.print("Display 1 Ready!");
  flushDisplay1(true);

  display2.setTextSize(1);
  display2.setTextColor(1);
  display2.setCursor(0, 10);
  display2.print("Display 2 Ready!");
  flushDisplay2(true);

  // Note: start in MENU state. Game initialization happens when entering game.

  // Initialize Serial only to display MAC addresses (disable other debug)
  Serial.begin(115200);
  delay(10);

  // Ensure WiFi STA is started so we can read the real MAC (matches ESPNOW_A.ino behavior)
  WiFi.mode(WIFI_STA);
  esp_err_t _r = esp_wifi_start();
  if (_r != ESP_OK) DBG_PRINTF("esp_wifi_start() returned %d\n", _r);

  // Print local MAC using esp_wifi_get_mac (reliable even before other WiFi activity)
  uint8_t mymac[6];
  if (esp_wifi_get_mac(WIFI_IF_STA, mymac) == ESP_OK) {
    Serial.printf("Local MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mymac[0], mymac[1], mymac[2], mymac[3], mymac[4], mymac[5]);
  } else {
    // fallback to Arduino API if esp_wifi_get_mac fails
    Serial.print("Local MAC: ");
    Serial.println(WiFi.macAddress());
  }

  char peerBuf[18];
  sprintf(peerBuf, "%02X:%02X:%02X:%02X:%02X:%02X", peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
  Serial.print("Peer MAC:  "); Serial.println(peerBuf);

  // Networking: initialize ESP-NOW and configure peer (set peer_mac above first)
  initEspNow();
  setPeerMac(peer_mac);
  addEspNowPeer();
  // buttons
  setupButtons();
  // show menu at startup
  enterMenu();
}

// Score hook called by game_engine when a breakable is destroyed
void addScore(uint8_t owner, int points) {
  // debug: print attribution info
  DBG_PRINTF("addScore: owner=%u myPlayerId=%u points=%d\n", owner, myPlayerId, points);
  if (owner == myPlayerId) {
    score_local += points;
    // send score update to peer so both devices display the same scores
    send_score_update(owner, (int16_t)points, myPlayerId);
  } else {
    score_remote += points;
  }
  // keep legacy 'score' variable in sync for any old code that reads it
  score = score_local;
  DBG_PRINTF("scores after addScore: local=%ld remote=%ld\n", score_local, score_remote);
}

void resetScores() {
  score_local = 0;
  score_remote = 0;
  score = score_local;
}

// -- Game packet handlers (called from espnow_game parser)
void game_on_input(const uint8_t *src_mac, const MsgInput *m) {
  if (!m) return;
  uint8_t f = m->inputFlags;
  if (!otherPlayerVisible) {
    if (m->h.fromId == 0) { otherPlayerX = 1; otherPlayerY = 1; }
    else { otherPlayerX = MAP_COLS - 2; otherPlayerY = MAP_ROWS - 2; }
    otherPlayerVisible = true;
  }
  int nx = otherPlayerX;
  int ny = otherPlayerY;
  if (f & 0x01) ny--;
  if (f & 0x02) ny++;
  if (f & 0x04) nx--;
  if (f & 0x08) nx++;
  if (nx >= 0 && nx < MAP_COLS && ny >= 0 && ny < MAP_ROWS) {
    if (mapData[ny][nx] == TILE_EMPTY) { otherPlayerX = nx; otherPlayerY = ny; }
  }
  // remote bomb visual (authoritative bomb should arrive via MSG_BOMB_PLACE)
  DBG_PRINT("RX INPUT flags="); DBG_PRINTLN(f);
}

void game_on_pos(const uint8_t *src_mac, const MsgPos *m) {
  if (!m) return;
  otherPlayerX = m->px;
  otherPlayerY = m->py;
  otherPlayerVisible = true;
}

void game_on_bomb_place(const uint8_t *src_mac, const MsgBombPlace *m) {
  if (!m) return;
  // The sender transmits the age (ms since placement) instead of its
  // absolute millis() to avoid requiring synchronized clocks. Interpret
  // m->placedMs as "age" here.
  unsigned long age = (unsigned long)m->placedMs;
  if (age >= (unsigned long)m->fuseMs) {
    unsigned long delta = age - (unsigned long)m->fuseMs;
    if (delta <= BOMB_STALE_THRESHOLD_MS) {
      unsigned long placedAt = millis() - (m->fuseMs - BOMB_MIN_REMAIN_MS);
      for (int i = 0; i < MAX_BOMBS; i++) {
        if (!bombs[i].active) {
          bombs[i].active = true;
          bombs[i].x = m->x;
          bombs[i].y = m->y;
          bombs[i].fuseMs = m->fuseMs;
          bombs[i].placedAt = placedAt;
          bombs[i].owner = m->h.fromId;
          DBG_PRINT("RX BOMB PLACE (slightly expired, scheduling) id="); DBG_PRINTLN(m->bombId);
          return;
        }
      }
      return;
    } else {
      DBG_PRINT("RX BOMB PLACE (stale) id="); DBG_PRINTLN(m->bombId);
      explodeAt(m->x, m->y, m->h.fromId);
      return;
    }
  }
  for (int i = 0; i < MAX_BOMBS; i++) {
    if (!bombs[i].active) {
      bombs[i].active = true;
      bombs[i].x = m->x;
      bombs[i].y = m->y;
      bombs[i].fuseMs = m->fuseMs;
      bombs[i].placedAt = millis() - age;
      // attribute this remote bomb to the sender
      bombs[i].owner = m->h.fromId;
      DBG_PRINT("RX BOMB PLACE id="); DBG_PRINT(m->bombId);
      DBG_PRINT(" age="); DBG_PRINT(age);
      DBG_PRINT(" fuse="); DBG_PRINTLN(m->fuseMs);
      return;
    }
  }
}

void game_on_bomb_explode(const uint8_t *src_mac, const MsgBombExplode *m) {
  if (!m) return;
  DBG_PRINT("RX BOMB EXPLODE id="); DBG_PRINTLN(m->bombId);
  explodeAt(m->cx, m->cy, m->h.fromId);
}

// Score update received from peer
void game_on_score_update(const uint8_t *src_mac, const MsgScoreUpdate *m) {
  if (!m) return;
  DBG_PRINTF("RX SCORE UPDATE owner=%u delta=%d from=%u\n", m->owner, m->delta, m->h.fromId);
  if (m->owner == myPlayerId) score_local += m->delta;
  else score_remote += m->delta;
  score = score_local;
  DBG_PRINTF("scores after RX: local=%ld remote=%ld\n", score_local, score_remote);
}

// Player death reported by peer (or by local device as broadcast)
void game_on_player_death(const uint8_t *src_mac, const MsgPlayerDeath *m) {
  if (!m) return;
  DBG_PRINTF("RX PLAYER DEATH victim=%u killer=%u from=%u s0=%ld s1=%ld\n", m->victimId, m->killerId, m->h.fromId, (long)m->score0, (long)m->score1);
  // Apply authoritative snapshot from the sender. Map score0/score1 into local
  // score_local/score_remote depending on our myPlayerId.
  if (myPlayerId == 0) {
    score_local = m->score0;
    score_remote = m->score1;
  } else {
    score_local = m->score1;
    score_remote = m->score0;
  }
  score = score_local;
  DBG_PRINTF("scores after DEATH snapshot applied: local=%ld remote=%ld\n", score_local, score_remote);
}

// Called by game_engine when a local bomb is about to explode (weak hook implementation)
void on_local_bomb_exploded(int cx, int cy, int bombId) {
  send_bomb_explode(myPlayerId, (uint16_t)bombId, (uint8_t)cx, (uint8_t)cy, (uint32_t)millis());
}


void game_on_join(const uint8_t *src_mac, const GameHdr *h, const uint8_t *payload, int payloadLen) {
  (void)payload; (void)payloadLen;
  if (h->fromId == 0) { otherPlayerX = 1; otherPlayerY = 1; }
  else { otherPlayerX = MAP_COLS - 2; otherPlayerY = MAP_ROWS - 2; }
  otherPlayerVisible = true;
  send_pos(myPlayerId, (uint8_t)playerX, (uint8_t)playerY);
}

void game_on_heartbeat(const uint8_t *src_mac, const GameHdr *h) {
  (void)src_mac;
  // mark peer presence only if we're in the waiting state
  if (gameState == STATE_WAITING) {
    // if this is the first time we see their ready since entering waiting, reply once
    if (!peerReady) {
      peerReady = true;
      peerReadyAt = millis();
      // reply so the sender knows we saw them (quick two-way handshake)
      send_ready(myPlayerId);
      DBG_PRINT("RX READY (first) from "); DBG_PRINTLN(h->fromId);
    } else {
      // already marked ready; refresh timestamp only
      peerReadyAt = millis();
      DBG_PRINT("RX READY (refresh) from "); DBG_PRINTLN(h->fromId);
    }
  } else {
    DBG_PRINT("RX READY (ignored, not waiting) from "); DBG_PRINTLN(h->fromId);
  }
}

void loop() {
  unsigned long now = millis();

  // poll buttons (menuActive depends on gameState)
  pollButtonsAndSend(gameState == STATE_MENU);
  // (removed serial keyboard input handling to avoid Serial/DBG-based control)
  // Only run game updates and rendering when in GAME state
  if (gameState == STATE_GAME) {
    // update bombs (handle fuse expiration -> explosions)
    // addExplosionCell() now applies immediate damage when explosion cells are created
    if (gameOver) {
      showGameOver();
      drawReturnToMenuPrompt();
      delay(100); // quicker update when game over (more responsive)
      return;
    }
    updateBombs();

    // Retransmit active local bomb placements periodically so peers stay in sync
    for (int i = 0; i < MAX_BOMBS; i++) {
      if (!bombs[i].active) continue;
      if (bombs[i].owner != myPlayerId) continue;
      unsigned long nowSend = millis();
      if (nowSend - lastBombPlaceSent[i] >= BOMB_PLACE_RESEND_MS) {
        uint32_t age = (uint32_t)(millis() - bombs[i].placedAt);
        send_bomb_place(myPlayerId, i, bombs[i].x, bombs[i].y, age, bombs[i].fuseMs);
        lastBombPlaceSent[i] = nowSend;
      }
    }

  // Render gameplay view to the first display (centered on player)
  int mapPixelWidth = MAP_COLS * TILE_SIZE;
  int playerCenter = playerX * TILE_SIZE + TILE_SIZE / 2;
  int xOffset = playerCenter - 64; // center player in 128px view
  if (xOffset < 0) xOffset = 0;
  int maxOffset = mapPixelWidth - 128;
  if (maxOffset < 0) maxOffset = 0;
  if (xOffset > maxOffset) xOffset = maxOffset;

  // Draw native-size viewport (no sprite stretching) centered on player
  display1.clearDisplay();
  renderMapToDisplay(display1, xOffset);
  renderBombsAndExplosions(display1, xOffset);
  flushDisplay1();

  // Render dedicated HUD to the second display (title, hearts, scores)
  display2.clearDisplay();
  drawHUDRight(display2);
  flushDisplay2();
  } else if (gameState == STATE_ENDING) {
    showGameOver();
    drawReturnToMenuPrompt();
    delay(100);
  }
  
  // Waiting state handling (symmetric to LCDA): check for forced start or timeout
  if (gameState == STATE_WAITING) {
    // update status display
    display2.clearDisplay();
    display2.setTextSize(1);
    display2.setCursor(4, 8);
    display2.print("Status:");
    display2.setCursor(4, 20);
    if (peerReady) display2.print("Player ready!        ");
    else display2.print("Waiting for peer...  ");
    flushDisplay2(true);

    if (!countdownStarted && peerReady && localReady && peerReadyAt >= waitingStartedAt) {
      // both ready -> start countdown (ensure heartbeat was seen after we entered waiting)
      DBG_PRINT("COUNTDOWN START - peerReadyAt="); DBG_PRINT(peerReadyAt);
      DBG_PRINT(" waitingStartedAt="); DBG_PRINTLN(waitingStartedAt);
      countdownStarted = true;
      countdownSec = 3;
      lastCountdownUpdate = millis();
      // If player 0 sends a map seed we'll receive it; player 1 does not emit a seed
    }

    if (countdownStarted) {
      if (millis() - lastCountdownUpdate >= 1000) {
        lastCountdownUpdate += 1000;
        countdownSec--;
      }
      display1.clearDisplay();
      display1.setTextSize(4);
      display1.setCursor(40, 32);
      if (countdownSec > 0) display1.print(countdownSec);
      else display1.print("GO");
      flushDisplay1(true);

      if (countdownSec <= 0) {
        delay(200); // shorten countdown delay
        enterGame();
        return;
      }
    }

    uint8_t waitBtns = sampleButtonsDebounced();
    // detect edge: require button pressed now but not at the moment we entered waiting
    bool startPressedNow = (waitBtns & 0x10) != 0 && !(waitingLastBtnFlags & 0x10);
    // update last-button snapshot for next poll
    waitingLastBtnFlags = waitBtns;
    if (startPressedNow) {
      // user forced start (edge)
      DBG_PRINTLN("WAITING: start forced by button (edge)");
      enterGame();
      return;
    }

    if (!peerReady && (millis() - waitingStartedAt >= WAIT_FOR_PEER_MS)) {
      DBG_PRINTLN("WAITING: timed out, starting solo");
      enterGame();
      return;
    }
    // skip regular game update while waiting
    return;
  }
}

void enterWaiting() {
  gameState = STATE_WAITING;
  localReady = true;
  peerReady = false;
  peerReadyAt = 0;
  countdownStarted = false;
  waitingStartedAt = millis();
  send_ready(myPlayerId);
  // record current button state so the press that entered waiting doesn't immediately force start
  waitingLastBtnFlags = sampleButtonsDebounced();
  DBG_PRINT("WAITING: started at "); DBG_PRINTLN(waitingStartedAt);
  display1.clearDisplay();
  display1.setTextSize(2);
  display1.setTextColor(1);
  display1.setCursor(8, 20);
  display1.print("WAITING");
  display1.setTextSize(1);
  display1.setCursor(8, 52);
  int waitingFor = (myPlayerId == 0) ? 2 : 1;
  display1.print("Waiting Player "); display1.print(waitingFor);
  flushDisplay1(true);
  display2.clearDisplay();
  display2.setTextSize(1); display2.setCursor(4, 8); display2.print("Status:");
  display2.setCursor(4, 20); display2.print("Ready sent..."); flushDisplay2(true);
}

// helper: draw a bitmap from PROGMEM scaled to destination rect using nearest-neighbor
static void drawSpriteScaled(Adafruit_SH1107 &disp, const uint8_t *bmp, int bw, int bh, int destX, int destY, int destW, int destH) {
  if (!bmp || bw <= 0 || bh <= 0 || destW <= 0 || destH <= 0) return;
  for (int dy = 0; dy < destH; dy++) {
    int sy = (dy * bh) / destH;
    for (int dx = 0; dx < destW; dx++) {
      int sx = (dx * bw) / destW;
      uint8_t row = pgm_read_byte(&bmp[sy]);
      int bitIndex = 7 - sx; if (bitIndex < 0) bitIndex = 0;
      bool on = (row >> bitIndex) & 0x01;
      if (on) disp.drawPixel(destX + dx, destY + dy, 1);
    }
  }
}

// Render the entire map scaled to exactly fill the 128x128 display (non-uniform integer scaling)
// This makes the arena occupy the full LCD area. Tiles may be stretched if MAP_ROWS != MAP_COLS.
void renderFullMapView(Adafruit_SH1107 &disp) {
  int scaleX = max(1, 128 / MAP_COLS); // horizontal pixels per tile
  int scaleY = max(1, 128 / MAP_ROWS); // vertical pixels per tile
  int mapW = MAP_COLS * scaleX;
  int mapH = MAP_ROWS * scaleY;
  int x0 = (128 - mapW) / 2; if (x0 < 0) x0 = 0;
  int y0 = (128 - mapH) / 2; if (y0 < 0) y0 = 0;

  // tiles (use 8x8 sprites stretched to tile rect)
  for (int ry = 0; ry < MAP_ROWS; ry++) {
    for (int cx = 0; cx < MAP_COLS; cx++) {
      int tx = x0 + cx * scaleX;
      int ty = y0 + ry * scaleY;
      Tile t = mapData[ry][cx];
      if (t == TILE_EMPTY) continue;
      if (t == TILE_SOLID) {
        drawSpriteScaled(disp, SPRITE_SOLID_8x8, 8, 8, tx, ty, scaleX, scaleY);
      } else if (t == TILE_BREAKABLE) {
        drawSpriteScaled(disp, SPRITE_BREAK_8x8, 8, 8, tx, ty, scaleX, scaleY);
      }
    }
  }

  // bombs (scale bomb sprite to tile size)
  for (int i = 0; i < MAX_BOMBS; i++) {
    if (!bombs[i].active) continue;
    int bx = x0 + bombs[i].x * scaleX;
    int by = y0 + bombs[i].y * scaleY;
    drawSpriteScaled(disp, SPRITE_BOMB_6x6, 6, 6, bx, by, scaleX, scaleY);
  }

  // explosions
  unsigned long now = millis();
  for (int i = 0; i < MAX_EXPLOSION_CELLS; i++) {
    if (explosions[i].endAt == 0) continue;
    if (now > explosions[i].endAt) continue;
    int ex = x0 + explosions[i].x * scaleX;
    int ey = y0 + explosions[i].y * scaleY;
    drawSpriteScaled(disp, SPRITE_EXPLODE_8x8, 8, 8, ex, ey, scaleX, scaleY);
  }

  // local player
  int ppx = x0 + playerX * scaleX;
  int ppy = y0 + playerY * scaleY;
  drawSpriteScaled(disp, SPRITE_PLAYER_6x6, 6, 6, ppx, ppy, scaleX, scaleY);

  // remote player
  if (otherPlayerVisible) {
    int opx = x0 + otherPlayerX * scaleX;
    int opy = y0 + otherPlayerY * scaleY;
    drawSpriteScaled(disp, SPRITE_PLAYER_6x6, 6, 6, opx, opy, scaleX, scaleY);
    // outline remote player box for contrast
    disp.drawRect(opx, opy, scaleX, scaleY, 1);
  }
}