#pragma once

#include <Arduino.h>
#include "SystemTypes.h"

inline const char *relayModeToText(RelayMode mode) {
  switch (mode) {
    case RelayMode::OFF:
      return "OFF";
    case RelayMode::ON:
      return "ON";
    case RelayMode::AUTO:
      return "AUTO";
    default:
      return "AUTO";
  }
}

inline const char *relayStateToText(RelayState state) {
  return state == RelayState::ON ? "ON" : "OFF";
}

inline const char *sourceToText(ControlSource source) {
  switch (source) {
    case ControlSource::MANUAL:
      return "MANUAL";
    case ControlSource::TIMER:
      return "TIMER";
    case ControlSource::PIR:
      return "PIR";
    default:
      return "NONE";
  }
}

inline const char *dayPhaseToText(DayPhase phase) {
  return phase == DayPhase::DAY ? "DAY" : "NIGHT";
}

inline RelayMode relayModeFromText(const String &text) {
  if (text.equalsIgnoreCase("ON")) {
    return RelayMode::ON;
  }
  if (text.equalsIgnoreCase("OFF")) {
    return RelayMode::OFF;
  }
  return RelayMode::AUTO;
}

inline RelayState relayStateFromText(const String &text) {
  if (text.equalsIgnoreCase("ON")) {
    return RelayState::ON;
  }
  return RelayState::OFF;
}

