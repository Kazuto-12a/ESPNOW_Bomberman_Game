// menu.h - minimal non-blocking main menu helpers
#pragma once

#include <Arduino.h>
#include <Adafruit_SH110X.h>

inline void showMainMenu(Adafruit_SH1107 &d1, Adafruit_SH1107 &d2) {
  d1.clearDisplay();
  d1.setTextSize(2); d1.setTextColor(1); d1.setCursor(16, 8);
  d1.print("MAIN MENU"); d1.drawRect(8, 36, 112, 72, 1);
  d1.setTextSize(1); d1.setCursor(20, 48); d1.print("  Start");
  d1.setCursor(20, 64); d1.print("  Settings"); d1.display();

  d2.clearDisplay(); d2.setTextSize(1); d2.setTextColor(1); d2.setCursor(4, 8);
  d2.print("Status:"); d2.drawRect(2, 30, 124, 86, 1);
  d2.setCursor(4, 20); d2.print("Connecting to peers..."); d2.display();
}

inline void showStartupMenu(Adafruit_SH1107 &d1, Adafruit_SH1107 &d2) { showMainMenu(d1, d2); }

inline void updateMenuStatus(Adafruit_SH1107 &d2, bool connected) {
  d2.fillRect(4, 18, 120, 20, 0);
  d2.setTextSize(1); d2.setTextColor(1); d2.setCursor(4, 20);
  if (connected) d2.print("Player 2: Connected   "); else d2.print("Connecting to peers...");
  d2.display();
}

// Provided by the networking layer (espnow_net.h)
extern bool isPlayer2Connected();

inline void configureMenuButtons() { }
inline void setMenuButtonPins(int /*startPin*/, int /*retryPin*/, int /*continuePin*/) { }
inline bool readButtonPressed(int /*pin*/) { return false; }

// End of menu.h
