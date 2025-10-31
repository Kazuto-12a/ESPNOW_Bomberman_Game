// sprites.h - monochrome sprite bitmaps (PROGMEM)
#pragma once

#include <Arduino.h>
#if defined(ARDUINO_ARCH_AVR)
#include <avr/pgmspace.h>
#else
#include <pgmspace.h>
#endif

// 8x8 filled block (solid)
const uint8_t SPRITE_SOLID_8x8[] PROGMEM = {
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF
};

// 8x8 breakable block (outline + decorative pattern)
const uint8_t SPRITE_BREAK_8x8[] PROGMEM = {
  0xFF, // 11111111
  0x81, // 10000001
  0x99, // 10011001
  0xA5, // 10100101
  0xA5, // 10100101
  0x99, // 10011001
  0x81, // 10000001
  0xFF  // 11111111
};

// 6x6 player sprite
const uint8_t SPRITE_PLAYER_6x6[] PROGMEM = {
  0x3C, // 00111100
  0x42, // 01000010
  0x5A, // 01011010 (eyes + cheek)
  0x5A, // 01011010
  0x42, // 01000010
  0x3C  // 00111100
};

// 6x6 bomb sprite
const uint8_t SPRITE_BOMB_6x6[] PROGMEM = {
  0x3C, // 00111100
  0x7E, // 01111110
  0x7E, // 01111110
  0x7E, // 01111110
  0x7E, // 01111110
  0x3C  // 00111100
};

// 8x8 explosion burst
const uint8_t SPRITE_EXPLODE_8x8[] PROGMEM = {
  0x18, // 00011000
  0x3C, // 00111100
  0x7E, // 01111110
  0xFF, // 11111111
  0xFF, // 11111111
  0x7E, // 01111110
  0x3C, // 00111100
  0x18  // 00011000
};

// 6x6 life icon
const uint8_t SPRITE_LIFE_6x6[] PROGMEM = {
  0x66, // 01100110
  0xFF, // 11111111
  0xFF, // 11111111
  0x7E, // 01111110
  0x3C, // 00111100
  0x18  // 00011000
};

// 8x6 life icon (avoids clipping at edges)
const uint8_t SPRITE_LIFE_8x6[] PROGMEM = {
  0x66, // 01100110
  0xFF, // 11111111
  0xFF, // 11111111
  0x7E, // 01111110
  0x3C, // 00111100
  0x18  // 00011000
};

// End of sprites.h
