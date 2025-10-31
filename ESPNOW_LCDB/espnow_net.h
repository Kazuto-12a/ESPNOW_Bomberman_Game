#pragma once

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// Lightweight ESP-NOW helper
static const uint8_t ESPNOW_PKT_PING = 0xA1;
static const uint8_t ESPNOW_PKT_PONG = 0xA2;
static uint8_t espnow_peer_mac[6] = {0,0,0,0,0,0};
static volatile uint32_t espnow_pending_nonce = 0;
static volatile bool espnow_got_pong = false;
static uint8_t espnow_last_pong_from[6] = {0};

extern void game_packet_received(const uint8_t *src_mac, const uint8_t *data, int len) __attribute__((weak));
inline uint8_t *espnow_get_peer_mac() { return espnow_peer_mac; }

inline void espnowOnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) { (void)info; (void)status; }

inline void espnowOnDataRecv(const esp_now_recv_info *recvInfo, const uint8_t *data, int len) {
  if (!recvInfo || !data || len <= 0) return;
  const uint8_t *src = recvInfo->src_addr;
  if (!src) return;
  if (len >= 5) {
    uint8_t typ = data[0]; uint32_t nonce = 0; memcpy(&nonce, data + 1, sizeof(uint32_t));
    if (typ == ESPNOW_PKT_PING) { 
      uint8_t pong[5]; pong[0]=ESPNOW_PKT_PONG; memcpy(pong+1,&nonce,4); 
      esp_now_send((uint8_t*)src, pong, sizeof(pong)); return; 
    }
    if (typ == ESPNOW_PKT_PONG) { 
      if (espnow_pending_nonce!=0 && nonce==espnow_pending_nonce) { 
        espnow_got_pong=true; memcpy((void*)espnow_last_pong_from, src, 6); espnow_pending_nonce=0; 
      } 
    }
  }
  if ((void*)game_packet_received != nullptr) game_packet_received(src, data, len);
}

inline void initEspNow() { WiFi.mode(WIFI_STA); esp_wifi_start(); if (esp_now_init() != ESP_OK) return; esp_now_register_send_cb(espnowOnDataSent); esp_now_register_recv_cb(espnowOnDataRecv); }
inline void setPeerMac(const uint8_t mac[6]) { if (!mac) return; memcpy(espnow_peer_mac, mac, 6); }
inline bool addEspNowPeer() { bool allZero=true; for (int i=0;i<6;i++) if (espnow_peer_mac[i]!=0) { allZero=false; break; } if (allZero) return false; esp_now_peer_info_t peerInfo={}; memcpy(peerInfo.peer_addr, espnow_peer_mac, 6); peerInfo.channel=0; peerInfo.encrypt=false; peerInfo.ifidx=WIFI_IF_STA; esp_err_t r=esp_now_add_peer(&peerInfo); return (r==ESP_OK||r==ESP_ERR_ESPNOW_EXIST); }

inline bool isPeerReachable(uint32_t timeoutMs=800) {
  bool allZero=true; for (int i=0;i<6;i++) if (espnow_peer_mac[i]!=0) { allZero=false; break; } if (allZero) return false;
  uint8_t pkt[5]; pkt[0]=ESPNOW_PKT_PING; uint32_t nonce=(uint32_t)micros(); if (nonce==0) nonce=1; memcpy(pkt+1,&nonce,4);
  espnow_pending_nonce=nonce; espnow_got_pong=false;
  esp_err_t r = esp_now_send(espnow_peer_mac, pkt, sizeof(pkt)); if (r!=ESP_OK) { espnow_pending_nonce=0; return false; }
  unsigned long start=millis(); while (millis()-start < timeoutMs) { if (espnow_got_pong) return true; delay(5); }
  espnow_pending_nonce = 0; return false;
}

inline bool isPlayer2Connected() { if (esp_now_init() != ESP_OK) { } return isPeerReachable(800); }
