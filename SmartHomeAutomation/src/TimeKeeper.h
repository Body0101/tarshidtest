#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "SystemTypes.h"

class TimeKeeper {
 public:
  void begin(Preferences *prefs);
  bool syncFromClient(uint64_t epochSeconds);
  bool syncFromHms(int hour, int minute, int second);
  bool syncFromDateTime(int year, int month, int day, int hour, int minute, int second);
  bool setTimezoneOffsetMinutes(int32_t offsetMinutes);
  bool trySyncFromNtp(bool force = false);
  uint64_t nowEpoch() const;
  uint64_t nowUserEpoch() const;
  bool hasValidTime() const;
  bool hasUserTime() const;
  DayPhase currentDayPhase() const;
  uint32_t currentDayToken() const;
  void maybePersistSyncPoint();
  void persistUserSyncPoint();

 private:
  void persistEpoch(uint64_t epochSeconds);
  void persistUserEpoch(uint64_t epochSeconds);
  void setEpoch(uint64_t epochSeconds);
  bool applyTimezoneRule(const String &tzRule, bool persist);
  bool buildTimeStruct(uint64_t epochSeconds, struct tm *out) const;

  Preferences *prefs_ = nullptr;
  bool timeValid_ = false;
  bool userTimeValid_ = false;
  uint32_t lastNtpAttemptMs_ = 0;
  uint64_t lastPersistedEpoch_ = 0;
};
