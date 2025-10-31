// game_engine.h - core game engine (map, bombs, explosions)
#pragma once

#include <Arduino.h>
#include "sprites.h"
#include <Adafruit_SH110X.h>
// debug macros (ENABLE_DEBUG may be defined in the main sketch)
#include "debug.h"

// Requirements (the main sketch must define these before including):
// TILE_SIZE, MAP_ROWS, MAP_COLS, HUD_HEIGHT and enum Tile.

// Map storage (defined in the sketch)
extern Tile mapData[MAP_ROWS][MAP_COLS];

// Bomb structure and storage (bombs[] defined in the sketch)
// Added owner field so bombs can be attributed to a player.
struct BombGE {
  bool active;
  int x;
  int y;
  unsigned long placedAt;
  unsigned long fuseMs;
  uint8_t owner; // player id who placed the bomb (0/1), 0xFF = unknown
};
typedef BombGE Bomb;
extern const int MAX_BOMBS;
extern Bomb bombs[];

// Explosion cell (visual) and storage (explosions[] defined in the sketch)
struct ExplosionCellGE { int x; int y; unsigned long endAt; };
typedef ExplosionCellGE ExplosionCell;
extern const int MAX_EXPLOSION_CELLS;
extern ExplosionCell explosions[];

// Parameters
extern const unsigned long BOMB_FUSE;
extern const unsigned long EXPLOSION_VIS_MS;
extern const int EXPLOSION_RADIUS;

// HUD/state (extern in sketch)
extern int lives;
extern long score;
extern int playerX;
extern int playerY;
extern int playerHealth;
// Shared timing for player invulnerability (defined in sketch)
extern unsigned long lastPlayerHitAt;
extern const unsigned long PLAYER_INVUL_MS;
// spawn invulnerability (defined in sketch)
extern const unsigned long SPAWN_INVUL_MS;
extern unsigned long spawnInvulEnd;
// explosion event counter (defined in sketch)
extern int explosionEventCounter;
// per-player spawn coordinates (defined in sketch)
extern int spawnX;
extern int spawnY;

// The main sketch may define these optional globals to control map behavior:
//   - const bool AUTO_RANDOMIZE_ON_START  : if true, randomize map at initializeGame();
//   - unsigned long MAP_SEED              : if non-zero, use this seed to deterministically
//                                         generate the map (both sketches can set the same seed)
extern const bool AUTO_RANDOMIZE_ON_START;
extern const unsigned long MAP_SEED;

// runtime-provided map seed (optionally set by the sketch before calling initializeGame)
extern unsigned long pending_map_seed;

// Score handling hook: implement this in the sketch to credit points to the
// appropriate player. If not implemented, game_engine falls back to a single
// global 'score' variable (legacy behavior).
extern void addScore(uint8_t owner, int points) __attribute__((weak));
extern void resetScores() __attribute__((weak));

// The main sketch should provide `myPlayerId` so the engine can attribute
// bombs placed locally. Declare as extern here.
extern uint8_t myPlayerId;

// Core game functions (implemented inline below)
// forceDamage: when true, damage is applied even if player invulnerability timer active.
// ownerId is the player id who caused the explosion (0/1) or 0xFF if unknown.
void addExplosionCell(int x, int y, uint8_t ownerId, bool forceDamage = false, int eventId = 0);
// damagePlayerAt is implemented in the main sketch; called when an explosion cell appears
// forceDamage: when true the damage call should bypass temporary invulnerability.
void damagePlayerAt(int x, int y, uint8_t ownerId, bool forceDamage = false, int eventId = 0);
// explodeAt now accepts an owner id so scoring can be attributed correctly.
void explodeAt(int bx, int by, uint8_t ownerId);
void updateBombs();
void placeBombAtPlayer();
void generateMap();
void renderBombsAndExplosions(Adafruit_SH1107 &disp, int xPixelOffset);
void drawTile(Adafruit_SH1107 &disp, int px, int py, Tile t);

// helpers
void initializeGame();
void randomizeMap();
bool isExplosionAt(int tx, int ty);
void checkPlayerHit();
// weak hook called when a local bomb is about to explode: implement in sketch to notify peers
extern void on_local_bomb_exploded(int cx, int cy, int bombId) __attribute__((weak));

// -----------------------------
// Implementations (inline)
// -----------------------------

inline void addExplosionCell(int x, int y, uint8_t ownerId, bool forceDamage, int eventId) {
  for (int i = 0; i < MAX_EXPLOSION_CELLS; i++) {
    if (explosions[i].endAt == 0 || millis() > explosions[i].endAt) {
      explosions[i].x = x;
      explosions[i].y = y;
      explosions[i].endAt = millis() + EXPLOSION_VIS_MS;
      // delegate damage handling to the main sketch implementation so
      // immunity rules (e.g., standing on own bomb) and game-over can be applied there
      damagePlayerAt(x, y, ownerId, forceDamage, eventId);
      return;
    }
  }
}

inline void explodeAt(int bx, int by, uint8_t ownerId) {
  // bump global event id so damage is applied only once per explosion event
  int ev = ++explosionEventCounter;
  DBG_PRINTF("explodeAt: bx=%d by=%d ownerId=%u\n", bx, by, ownerId);
  // center (force damage so players standing on the exploding bomb are affected)
  addExplosionCell(bx, by, ownerId, true, ev);
  // apply breakable destruction at center. Attribute points to ownerId via addScore hook when available.
  if (mapData[by][bx] == TILE_BREAKABLE) {
    mapData[by][bx] = TILE_EMPTY;
    // Prefer the bomb's recorded owner if available (more robust than relying solely on ownerId)
    uint8_t ownerToCredit = ownerId;
    int matchedIdx = -1;
    for (int bi = 0; bi < MAX_BOMBS; bi++) {
      if (bombs[bi].x == bx && bombs[bi].y == by && bombs[bi].owner != (uint8_t)0xFF) {
        ownerToCredit = bombs[bi].owner;
        matchedIdx = bi;
        break;
      }
    }
    DBG_PRINTF("explodeAt:center destroyed (%d,%d) ownerParam=%u ownerToCredit=%u matchedIdx=%d\n", bx, by, ownerId, ownerToCredit, matchedIdx);
    // Only the authoritative device (the one that placed the bomb) should
    // apply and broadcast score changes. If we have a matching bomb record
    // and it belongs to us, apply/addScore and notify peer. Otherwise do
    // not apply locally — the authoritative device will send a score update.
    if (matchedIdx != -1 && bombs[matchedIdx].owner == myPlayerId) {
      if ((void*)addScore != nullptr) addScore(ownerToCredit, 10);
      else score += 10;
      // notify peer
      send_score_update(ownerToCredit, (int16_t)10, myPlayerId);
    }
  }
  const int dx[4] = {1, -1, 0, 0};
  const int dy[4] = {0, 0, 1, -1};
  for (int d = 0; d < 4; d++) {
    for (int r = 1; r <= EXPLOSION_RADIUS; r++) {
      int nx = bx + dx[d]*r;
      int ny = by + dy[d]*r;
      if (nx < 0 || nx >= MAP_COLS || ny < 0 || ny >= MAP_ROWS) break;
      if (mapData[ny][nx] == TILE_SOLID) break;
      // If it's a breakable tile, destroy it and stop propagation. Do this
      // before creating the explosion cell to avoid any race where damage
      // handlers modify mapData during the addExplosionCell call.
      if (mapData[ny][nx] == TILE_BREAKABLE) {
        mapData[ny][nx] = TILE_EMPTY;
        // Prefer a bomb owner at that tile if present
        uint8_t ownerToCredit = ownerId;
        int matchedIdx = -1;
        for (int bi = 0; bi < MAX_BOMBS; bi++) {
          if (bombs[bi].x == nx && bombs[bi].y == ny && bombs[bi].owner != (uint8_t)0xFF) {
            ownerToCredit = bombs[bi].owner;
            matchedIdx = bi;
            break;
          }
        }
        DBG_PRINTF("explodeAt:prop destroyed (%d,%d) ownerParam=%u ownerToCredit=%u matchedIdx=%d\n", nx, ny, ownerId, ownerToCredit, matchedIdx);
        // create the explosion visual and apply damage at this tile
        addExplosionCell(nx, ny, ownerId, false, ev);
        // Only the authoritative device applies and broadcasts the score.
        if (matchedIdx != -1 && bombs[matchedIdx].owner == myPlayerId) {
          if ((void*)addScore != nullptr) addScore(ownerToCredit, 10);
          else score += 10;
          send_score_update(ownerToCredit, (int16_t)10, myPlayerId);
        }
        break;
      } else {
        // empty tile or temporary explosion passage: create explosion cell
        addExplosionCell(nx, ny, ownerId, false, ev);
      }
    }
  }
}

inline void updateBombs() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_BOMBS; i++) {
    if (!bombs[i].active) continue;
    if (now - bombs[i].placedAt >= bombs[i].fuseMs) {
      // mark the bomb inactive before exploding so any damage handlers
      // don't see the bomb as still 'present' on that tile
      bombs[i].active = false;
      bombs[i].placedAt = 0;
      // notify sketch (weak hook) that a local bomb exploded so it can send network messages
      if ((void*)on_local_bomb_exploded != nullptr) on_local_bomb_exploded(bombs[i].x, bombs[i].y, i);
      // pass the owner so scoring can be attributed correctly
      explodeAt(bombs[i].x, bombs[i].y, bombs[i].owner);
    }
  }
}

inline void placeBombAtPlayer() {
  for (int i = 0; i < MAX_BOMBS; i++) {
    if (bombs[i].active && bombs[i].x == playerX && bombs[i].y == playerY) return;
  }
  for (int i = 0; i < MAX_BOMBS; i++) {
    if (!bombs[i].active) {
      bombs[i].active = true;
      bombs[i].x = playerX;
      bombs[i].y = playerY;
      bombs[i].placedAt = millis();
      bombs[i].fuseMs = BOMB_FUSE;
      // attribute this bomb to the local player if available
      bombs[i].owner = (uint8_t)0xFF;
      if ((void*)&myPlayerId != nullptr) bombs[i].owner = myPlayerId;
      return;
    }
  }
}

inline void generateMap() {
  for (int r = 0; r < MAP_ROWS; r++) for (int c = 0; c < MAP_COLS; c++) mapData[r][c] = TILE_EMPTY;
  for (int r = 0; r < MAP_ROWS; r++) for (int c = 0; c < MAP_COLS; c++) if (r==0||r==MAP_ROWS-1||c==0||c==MAP_COLS-1) mapData[r][c]=TILE_SOLID;
  for (int r = 2; r < MAP_ROWS - 2; r += 2) for (int c = 2; c < MAP_COLS - 2; c += 2) mapData[r][c] = TILE_SOLID;
  for (int r = 1; r < MAP_ROWS - 1; r++) for (int c = 1; c < MAP_COLS - 1; c++) {
  if (mapData[r][c] == TILE_EMPTY) {
      // Reserve only a single safe tile in each corner (r==1,c==1 etc.) instead of a 2x2 area.
      bool inCorner = false;
      if ((r <= 1 && c <= 1) || (r <= 1 && c >= MAP_COLS - 2) || (r >= MAP_ROWS - 2 && c <= 1) || (r >= MAP_ROWS - 2 && c >= MAP_COLS - 2)) inCorner = true;
      if (!inCorner && random(100) < 50) mapData[r][c] = TILE_BREAKABLE;
    }
  }
  // Ensure spawn areas are clear: keep a 2x2 empty zone at each corner so
  // players don't spawn adjacent to destructible walls and die immediately.
  auto clearIfInBounds = [&](int rr, int cc) {
    if (rr > 0 && rr < MAP_ROWS-1 && cc > 0 && cc < MAP_COLS-1) mapData[rr][cc] = TILE_EMPTY;
  };
  // top-left
  clearIfInBounds(1,1); clearIfInBounds(1,2); clearIfInBounds(2,1); clearIfInBounds(2,2);
  // top-right
  clearIfInBounds(1, MAP_COLS-2); clearIfInBounds(1, MAP_COLS-3); clearIfInBounds(2, MAP_COLS-2); clearIfInBounds(2, MAP_COLS-3);
  // bottom-left
  clearIfInBounds(MAP_ROWS-2,1); clearIfInBounds(MAP_ROWS-2,2); clearIfInBounds(MAP_ROWS-3,1); clearIfInBounds(MAP_ROWS-3,2);
  // bottom-right
  clearIfInBounds(MAP_ROWS-2, MAP_COLS-2); clearIfInBounds(MAP_ROWS-2, MAP_COLS-3); clearIfInBounds(MAP_ROWS-3, MAP_COLS-2); clearIfInBounds(MAP_ROWS-3, MAP_COLS-3);
}

inline void renderBombsAndExplosions(Adafruit_SH1107 &disp, int xPixelOffset) {
  for (int i = 0; i < MAX_BOMBS; i++) {
    if (!bombs[i].active) continue;
  int bombPixelX = bombs[i].x * TILE_SIZE;
  int bombPixelY = bombs[i].y * TILE_SIZE + HUD_HEIGHT;
    if (bombPixelX >= xPixelOffset && bombPixelX < xPixelOffset + 128) {
      int px = bombPixelX - xPixelOffset + 1;
      int py = bombPixelY + 1;
      disp.drawBitmap(px, py, SPRITE_BOMB_6x6, 6, 6, 1);
    }
  }
  unsigned long now = millis();
  for (int i = 0; i < MAX_EXPLOSION_CELLS; i++) {
    if (explosions[i].endAt == 0) continue;
    if (now > explosions[i].endAt) continue;
  int ex = explosions[i].x * TILE_SIZE;
  int ey = explosions[i].y * TILE_SIZE + HUD_HEIGHT;
    if (ex >= xPixelOffset && ex < xPixelOffset + 128) {
      int px = ex - xPixelOffset;
      int py = ey;
      disp.drawBitmap(px, py, SPRITE_EXPLODE_8x8, 8, 8, 1);
    }
  }
}

inline void drawTile(Adafruit_SH1107 &disp, int px, int py, Tile t) {
  switch (t) {
    case TILE_EMPTY: break;
    case TILE_SOLID: disp.drawBitmap(px, py, SPRITE_SOLID_8x8, 8, 8, 1); break;
    case TILE_BREAKABLE: disp.drawBitmap(px, py, SPRITE_BREAK_8x8, 8, 8, 1); break;
  }
}

inline void initializeGame() {
  // Map generation behavior:
  // - If MAP_SEED != 0 use that seed for deterministic map (both devices can set same seed)
  // - Else if AUTO_RANDOMIZE_ON_START is true, randomizeMap() (uses floating A0 + millis())
  // - Else generateMap() without extra seeding
  if (MAP_SEED != 0) {
    randomSeed(MAP_SEED);
    generateMap();
  } else if (pending_map_seed != 0) {
    // use runtime seed provided by peer or authoritative device
    randomSeed(pending_map_seed);
    generateMap();
    // consume it so next games will re-generate
    pending_map_seed = 0;
  } else if (AUTO_RANDOMIZE_ON_START) {
    randomizeMap();
  } else {
    generateMap();
  }

  playerX = 1; playerY = 1; playerHealth = 1;
  // Clear any leftover bombs/explosions from previous rounds or menu actions so
  // a stale bomb does not immediately explode when the game starts.
  for (int i = 0; i < MAX_BOMBS; i++) {
    bombs[i].active = false;
    bombs[i].placedAt = 0;
    bombs[i].x = 0; bombs[i].y = 0; bombs[i].fuseMs = 0;
    bombs[i].owner = 0xFF;
  }
  for (int i = 0; i < MAX_EXPLOSION_CELLS; i++) {
    explosions[i].endAt = 0;
    explosions[i].x = 0; explosions[i].y = 0;
  }
  // reset explosion event counter so event ids start fresh for this round
  explosionEventCounter = 0;
  // reset score and lives for a new game
  lives = 3;
  if ((void*)resetScores != nullptr) resetScores();
  else score = 0;
  // ensure basic per-life health is set
  playerHealth = 1;
}

inline void randomizeMap() { randomSeed(analogRead(A0) ^ millis()); generateMap(); }

inline bool isExplosionAt(int tx, int ty) {
  unsigned long now = millis();
  for (int i = 0; i < MAX_EXPLOSION_CELLS; i++) {
    if (explosions[i].endAt == 0) continue;
    if (now > explosions[i].endAt) continue;
    if (explosions[i].x == tx && explosions[i].y == ty) return true;
  }
  return false;
}

inline void checkPlayerHit() {
  unsigned long now = millis();
  // legacy per-hit invul check (kept but non-essential)
  if (now - lastPlayerHitAt < PLAYER_INVUL_MS) return;
  if (isExplosionAt(playerX, playerY)) {
    // one explosion hit removes one heart
    if (lives > 0) lives--;
    lastPlayerHitAt = now;
    if (lives > 0) {
      playerHealth = 1;
      playerX = spawnX; playerY = spawnY;
      spawnInvulEnd = millis() + SPAWN_INVUL_MS;
    } else {
      // out of lives -> game over
      // set player to spawn and leave game over handling to main loop
      playerX = spawnX; playerY = spawnY;
    }
  }
}

// End of game_engine.h