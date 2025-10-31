#pragma once

// espnow_game.h - game message definitions and send helpers

#include <Arduino.h>
#include <esp_now.h>
#include "espnow_net.h"

// Message types
enum MsgType : uint8_t {
  MSG_JOIN = 1,
  MSG_JOIN_ACK = 2,
  MSG_HEARTBEAT = 3,
  MSG_INPUT = 4,
  MSG_POS = 5,
  MSG_BOMB_PLACE = 6,
  MSG_BOMB_EXPLODE = 7,
  MSG_MAP_SYNC = 8,
  MSG_STATE_SNAPSHOT = 9,
  MSG_SCORE_UPDATE = 10,
  MSG_PLAYER_DEATH = 11,
  MSG_ACK = 200
};

// Packet header
struct __attribute__((packed)) GameHdr { uint8_t type; uint16_t seq; uint8_t fromId; };

// Player input
struct __attribute__((packed)) MsgInput { GameHdr h; uint32_t clientTick; uint8_t inputFlags; uint8_t reserved; };

// Position update (unreliable)
struct __attribute__((packed)) MsgPos { GameHdr h; uint8_t px; uint8_t py; uint8_t dir; int8_t vx; int8_t vy; };

// Bomb placement (reliable)
struct __attribute__((packed)) MsgBombPlace { GameHdr h; uint16_t bombId; uint8_t x, y; uint32_t placedMs; uint16_t fuseMs; };

// Bomb explosion (reliable)
struct __attribute__((packed)) MsgBombExplode { GameHdr h; uint16_t bombId; uint8_t cx, cy; uint32_t explodeMs; };

// Score update (delta applied to the owner)
struct __attribute__((packed)) MsgScoreUpdate { GameHdr h; uint8_t owner; int16_t delta; };

// Player death: victim and killer ids + authoritative scores snapshot.
// Sender should be the device where the death occurred and it includes
// the updated absolute scores for player0 and player1 so peers can sync.
struct __attribute__((packed)) MsgPlayerDeath { GameHdr h; uint8_t victimId; uint8_t killerId; int32_t score0; int32_t score1; };

// ACK for reliable messages
struct __attribute__((packed)) MsgAck { GameHdr h; uint16_t ackSeq; uint8_t reserved; };

// Sequence generator
static uint16_t game_seq_counter = 1;
inline uint16_t next_game_seq() { return game_seq_counter++; }

// Weak handlers you can implement in your sketch to receive game events
extern void game_on_join(const uint8_t *src_mac, const GameHdr *h, const uint8_t *payload, int payloadLen) __attribute__((weak));
extern void game_on_input(const uint8_t *src_mac, const MsgInput *m) __attribute__((weak));
extern void game_on_bomb_place(const uint8_t *src_mac, const MsgBombPlace *m) __attribute__((weak));
extern void game_on_bomb_explode(const uint8_t *src_mac, const MsgBombExplode *m) __attribute__((weak));
extern void game_on_pos(const uint8_t *src_mac, const MsgPos *m) __attribute__((weak));
extern void game_on_score_update(const uint8_t *src_mac, const MsgScoreUpdate *m) __attribute__((weak));
extern void game_on_player_death(const uint8_t *src_mac, const MsgPlayerDeath *m) __attribute__((weak));
extern void game_on_state_snapshot(const uint8_t *src_mac, const uint8_t *data, int len) __attribute__((weak));
extern void game_on_ack(const uint8_t *src_mac, const MsgAck *m) __attribute__((weak));
extern void game_on_heartbeat(const uint8_t *src_mac, const GameHdr *h) __attribute__((weak));

// Send helpers (fire-and-forget; caller may add reliability wrappers)
inline bool send_raw_to_peer(const uint8_t *buf, size_t len) {
  uint8_t *peer = espnow_get_peer_mac();
  // refuse if peer not configured
  bool zero = true; for (int i=0;i<6;i++) if (peer[i]!=0) { zero=false; break; }
  if (zero) return false;
  esp_err_t r = esp_now_send(peer, buf, len);
  return (r == ESP_OK);
}

inline bool send_join(uint8_t fromId) {
  uint8_t pkt[sizeof(GameHdr)];
  GameHdr *h = (GameHdr*)pkt;
  h->type = MSG_JOIN; h->seq = next_game_seq(); h->fromId = fromId;
  return send_raw_to_peer(pkt, sizeof(pkt));
}

inline bool send_ack(uint16_t ackSeq, uint8_t fromId) {
  MsgAck m;
  m.h.type = MSG_ACK; m.h.seq = next_game_seq(); m.h.fromId = fromId;
  m.ackSeq = ackSeq; m.reserved = 0;
  return send_raw_to_peer((uint8_t*)&m, sizeof(m));
}

inline bool send_input(uint8_t fromId, uint32_t clientTick, uint8_t inputFlags) {
  MsgInput m;
  m.h.type = MSG_INPUT; m.h.seq = next_game_seq(); m.h.fromId = fromId;
  m.clientTick = clientTick; m.inputFlags = inputFlags; m.reserved = 0;
  return send_raw_to_peer((uint8_t*)&m, sizeof(m));
}

inline bool send_bomb_place(uint8_t fromId, uint16_t bombId, uint8_t x, uint8_t y, uint32_t placedMs, uint16_t fuseMs) {
  MsgBombPlace m;
  m.h.type = MSG_BOMB_PLACE; m.h.seq = next_game_seq(); m.h.fromId = fromId;
  m.bombId = bombId; m.x = x; m.y = y; m.placedMs = placedMs; m.fuseMs = fuseMs;
  return send_raw_to_peer((uint8_t*)&m, sizeof(m));
}

inline bool send_bomb_explode(uint8_t fromId, uint16_t bombId, uint8_t cx, uint8_t cy, uint32_t explodeMs) {
  MsgBombExplode m;
  m.h.type = MSG_BOMB_EXPLODE; m.h.seq = next_game_seq(); m.h.fromId = fromId;
  m.bombId = bombId; m.cx = cx; m.cy = cy; m.explodeMs = explodeMs;
  return send_raw_to_peer((uint8_t*)&m, sizeof(m));
}

inline bool send_ready(uint8_t fromId) {
  GameHdr h;
  h.type = MSG_HEARTBEAT; h.seq = next_game_seq(); h.fromId = fromId;
  return send_raw_to_peer((uint8_t*)&h, sizeof(h));
}

// Position update (unreliable)
inline bool send_pos(uint8_t fromId, uint8_t px, uint8_t py, uint8_t dir = 0, int8_t vx = 0, int8_t vy = 0) {
  MsgPos m;
  m.h.type = MSG_POS; m.h.seq = next_game_seq(); m.h.fromId = fromId;
  m.px = px; m.py = py; m.dir = dir; m.vx = vx; m.vy = vy;
  return send_raw_to_peer((uint8_t*)&m, sizeof(m));
}

inline bool send_score_update(uint8_t owner, int16_t delta, uint8_t fromId) {
  MsgScoreUpdate m;
  m.h.type = MSG_SCORE_UPDATE; m.h.seq = next_game_seq(); m.h.fromId = fromId;
  m.owner = owner; m.delta = delta;
  return send_raw_to_peer((uint8_t*)&m, sizeof(m));
}

inline bool send_player_death(uint8_t victimId, uint8_t killerId, int32_t score0, int32_t score1, uint8_t fromId) {
  MsgPlayerDeath m;
  m.h.type = MSG_PLAYER_DEATH; m.h.seq = next_game_seq(); m.h.fromId = fromId;
  m.victimId = victimId; m.killerId = killerId; m.score0 = score0; m.score1 = score1;
  return send_raw_to_peer((uint8_t*)&m, sizeof(m));
}

// State snapshot: beware of size; keep under 1400 bytes to avoid fragmentation issues
inline bool send_state_snapshot(const uint8_t *data, size_t len, uint8_t fromId) {
  if (len + sizeof(GameHdr) > 1450) return false; // avoid big packets here
  // allocate on stack only for small len; if bigger, caller should fragment
  uint8_t buf[1500];
  GameHdr *h = (GameHdr*)buf;
  h->type = MSG_STATE_SNAPSHOT; h->seq = next_game_seq(); h->fromId = fromId;
  memcpy(buf + sizeof(GameHdr), data, len);
  return send_raw_to_peer(buf, sizeof(GameHdr) + len);
}

// Parser: call this to parse raw buffer and dispatch to weak handlers
inline void processGamePacket(const uint8_t *src_mac, const uint8_t *data, int len) {
  if (!data || len < (int)sizeof(GameHdr)) return;
  const GameHdr *h = (const GameHdr*)data;
  switch (h->type) {
    case MSG_INPUT:
      if (len >= (int)sizeof(MsgInput)) {
        const MsgInput *m = (const MsgInput*)data;
        if ((void*)game_on_input != nullptr) game_on_input(src_mac, m);
      }
      break;
    case MSG_BOMB_PLACE:
      if (len >= (int)sizeof(MsgBombPlace)) {
        const MsgBombPlace *m = (const MsgBombPlace*)data;
        if ((void*)game_on_bomb_place != nullptr) game_on_bomb_place(src_mac, m);
      }
      break;
    case MSG_BOMB_EXPLODE:
      if (len >= (int)sizeof(MsgBombExplode)) {
        const MsgBombExplode *m = (const MsgBombExplode*)data;
        if ((void*)game_on_bomb_explode != nullptr) game_on_bomb_explode(src_mac, m);
      }
      break;
    case MSG_POS:
      if (len >= (int)sizeof(MsgPos)) {
        const MsgPos *m = (const MsgPos*)data;
        if ((void*)game_on_pos != nullptr) game_on_pos(src_mac, m);
      }
      break;
    case MSG_SCORE_UPDATE:
      if (len >= (int)sizeof(MsgScoreUpdate)) {
        const MsgScoreUpdate *m = (const MsgScoreUpdate*)data;
        if ((void*)game_on_score_update != nullptr) game_on_score_update(src_mac, m);
      }
      break;
    case MSG_PLAYER_DEATH:
      if (len >= (int)sizeof(MsgPlayerDeath)) {
        const MsgPlayerDeath *m = (const MsgPlayerDeath*)data;
        if ((void*)game_on_player_death != nullptr) game_on_player_death(src_mac, m);
      }
      break;
    case MSG_STATE_SNAPSHOT:
      if ((void*)game_on_state_snapshot != nullptr) {
        game_on_state_snapshot(src_mac, data + sizeof(GameHdr), len - sizeof(GameHdr));
      }
      break;
    case MSG_ACK:
      if (len >= (int)sizeof(MsgAck)) {
        const MsgAck *m = (const MsgAck*)data;
        if ((void*)game_on_ack != nullptr) game_on_ack(src_mac, m);
      }
      break;
    case MSG_JOIN:
    case MSG_JOIN_ACK:
      if ((void*)game_on_join != nullptr) game_on_join(src_mac, h, data + sizeof(GameHdr), len - sizeof(GameHdr));
      break;
    case MSG_HEARTBEAT:
      if ((void*)game_on_heartbeat != nullptr) game_on_heartbeat(src_mac, h);
      else if ((void*)game_on_join != nullptr) game_on_join(src_mac, h, data + sizeof(GameHdr), len - sizeof(GameHdr));
      break;
    default:
      // unknown type: ignore
      break;
  }
}

// Provide a concrete game_packet_received implementation so espnow_net can call into this parser
inline void game_packet_received(const uint8_t *src_mac, const uint8_t *data, int len) {
  processGamePacket(src_mac, data, len);
}
