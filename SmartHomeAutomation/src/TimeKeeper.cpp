#include "TimeKeeper.h"

#include <cstring>
#include <cstdlib>
#include <sys/time.h>
#include <WiFi.h>
#include <time.h>

#include "Config.h"

namespace {
constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.nist.gov";
constexpr uint32_t NTP_RETRY_MS = 45 * 1000;
constexpr uint32_t PERSIST_INTERVAL_SECONDS = 600;
constexpr int32_t MAX_TIMEZONE_OFFSET_MINUTES = 14 * 60;

String buildFixedOffsetTimezone(int32_t offsetMinutes) {
  // JavaScript sends getTimezoneOffset(): UTC - local time.
  // POSIX TZ strings invert that sign, so UTC+2 becomes "UTC-2".
  const int32_t safeOffset = constrain(offsetMinutes, -MAX_TIMEZONE_OFFSET_MINUTES, MAX_TIMEZONE_OFFSET_MINUTES);
  if (safeOffset == 0) {
    return "UTC0";
  }

  const int32_t absMinutes = abs(safeOffset);
  const int32_t hours = absMinutes / 60;
  const int32_t minutes = absMinutes % 60;
  const char sign = safeOffset > 0 ? '+' : '-';

  char buffer[20];
  if (minutes == 0) {
    snprintf(buffer, sizeof(buffer), "UTC%c%d", sign, static_cast<int>(hours));
  } else {
    snprintf(buffer, sizeof(buffer), "UTC%c%d:%02d", sign, static_cast<int>(hours), static_cast<int>(minutes));
  }
  return String(buffer);
}
}  // namespace

void TimeKeeper::begin(Preferences *prefs) {
  prefs_ = prefs;

  // Timezone is not hardcoded to a region. Use stored client-provided value when available,
  // otherwise keep environment/default TZ (or fall back to UTC once).
  String storedTz;
  if (prefs_) {
    storedTz = prefs_->getString("tz", "");
  }
  if (storedTz.length() > 0) {
    applyTimezoneRule(storedTz, false);
  } else if (!getenv("TZ")) {
    applyTimezoneRule("UTC0", false);
  }
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);

  if (prefs_) {
    const uint64_t storedUserEpoch = prefs_->getULong64("last_user_epoch", 0);
    if (storedUserEpoch > 1700000000ULL) {
      setEpoch(storedUserEpoch);
      userTimeValid_ = true;
      lastPersistedEpoch_ = storedUserEpoch;
      return;
    }

    const uint64_t storedEpoch = prefs_->getULong64("last_epoch", 0);
    if (storedEpoch > 1700000000ULL) {
      setEpoch(storedEpoch);
      lastPersistedEpoch_ = storedEpoch;
    }
  }
}

bool TimeKeeper::syncFromClient(uint64_t epochSeconds) {
  if (epochSeconds < 1700000000ULL) {
    return false;
  }
  setEpoch(epochSeconds);
  persistEpoch(epochSeconds);
  persistUserEpoch(epochSeconds);
  return true;
}

bool TimeKeeper::syncFromHms(int hour, int minute, int second) {
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
    return false;
  }

  // HMS-only sync must anchor to a previously user-synchronized date.
  uint64_t reference = nowUserEpoch();
  if (reference < 1700000000ULL && prefs_) {
    reference = prefs_->getULong64("last_user_epoch", 0);
  }
  if (reference < 1700000000ULL) {
    return false;
  }

  struct tm info;
  if (!buildTimeStruct(reference, &info)) {
    return false;
  }
  info.tm_hour = hour;
  info.tm_min = minute;
  info.tm_sec = second;
  info.tm_isdst = -1;

  const time_t raw = mktime(&info);
  if (raw <= 0) {
    return false;
  }

  setEpoch(static_cast<uint64_t>(raw));
  persistEpoch(static_cast<uint64_t>(raw));
  persistUserEpoch(static_cast<uint64_t>(raw));
  return true;
}

bool TimeKeeper::syncFromDateTime(int year, int month, int day, int hour, int minute, int second) {
  if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
      minute > 59 || second < 0 || second > 59) {
    return false;
  }

  struct tm info;
  memset(&info, 0, sizeof(info));
  info.tm_year = year - 1900;
  info.tm_mon = month - 1;
  info.tm_mday = day;
  info.tm_hour = hour;
  info.tm_min = minute;
  info.tm_sec = second;
  info.tm_isdst = -1;

  const time_t raw = mktime(&info);
  if (raw <= 0) {
    return false;
  }
  setEpoch(static_cast<uint64_t>(raw));
  persistEpoch(static_cast<uint64_t>(raw));
  persistUserEpoch(static_cast<uint64_t>(raw));
  return true;
}

bool TimeKeeper::setTimezoneOffsetMinutes(int32_t offsetMinutes) {
  if (offsetMinutes < -MAX_TIMEZONE_OFFSET_MINUTES || offsetMinutes > MAX_TIMEZONE_OFFSET_MINUTES) {
    return false;
  }
  return applyTimezoneRule(buildFixedOffsetTimezone(offsetMinutes), true);
}

bool TimeKeeper::trySyncFromNtp(bool force) {
  // Offline timers keep user-device time once available. Online/cloud mode can
  // force NTP so the ESP is no longer dependent on a local browser clock.
  if (userTimeValid_ && !force) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  const uint32_t nowMs = millis();
  if (nowMs - lastNtpAttemptMs_ < NTP_RETRY_MS) {
    return false;
  }
  lastNtpAttemptMs_ = nowMs;

  struct tm localTm;
  if (!getLocalTime(&localTm, 1500)) {
    return false;
  }
  const time_t raw = mktime(&localTm);
  if (raw <= 0) {
    return false;
  }
  setEpoch(static_cast<uint64_t>(raw));
  persistEpoch(static_cast<uint64_t>(raw));
  return true;
}

uint64_t TimeKeeper::nowEpoch() const {
  if (!timeValid_) {
    return 0;
  }
  const time_t raw = time(nullptr);
  if (raw <= 0) {
    return 0;
  }
  return static_cast<uint64_t>(raw);
}

uint64_t TimeKeeper::nowUserEpoch() const {
  if (!userTimeValid_) {
    return 0;
  }
  const uint64_t epoch = nowEpoch();
  if (epoch < 1700000000ULL) {
    return 0;
  }
  return epoch;
}

bool TimeKeeper::hasValidTime() const { return timeValid_; }
bool TimeKeeper::hasUserTime() const { return nowUserEpoch() >= 1700000000ULL; }

DayPhase TimeKeeper::currentDayPhase() const {
  if (!timeValid_) {
    return DayPhase::DAY;
  }
  struct tm info;
  if (!buildTimeStruct(nowEpoch(), &info)) {
    return DayPhase::DAY;
  }
  if (info.tm_hour >= DAY_START_HOUR && info.tm_hour < NIGHT_START_HOUR) {
    return DayPhase::DAY;
  }
  return DayPhase::NIGHT;
}

uint32_t TimeKeeper::currentDayToken() const {
  if (!timeValid_) {
    return 0;
  }
  struct tm info;
  if (!buildTimeStruct(nowEpoch(), &info)) {
    return 0;
  }
  return static_cast<uint32_t>((info.tm_year + 1900) * 10000 + (info.tm_mon + 1) * 100 + info.tm_mday);
}

void TimeKeeper::maybePersistSyncPoint() {
  if (!timeValid_) {
    return;
  }
  const uint64_t epoch = nowEpoch();
  if (epoch < 1700000000ULL) {
    return;
  }
  if (lastPersistedEpoch_ != 0 && (epoch - lastPersistedEpoch_) < PERSIST_INTERVAL_SECONDS) {
    return;
  }
  persistEpoch(epoch);
  if (userTimeValid_) {
    persistUserEpoch(epoch);
  }
}

void TimeKeeper::persistUserSyncPoint() {
  if (!userTimeValid_) {
    return;
  }
  const uint64_t epoch = nowUserEpoch();
  if (epoch < 1700000000ULL) {
    return;
  }
  persistEpoch(epoch);
  persistUserEpoch(epoch);
}

void TimeKeeper::setEpoch(uint64_t epochSeconds) {
  struct timeval tv;
    tv.tv_sec = static_cast<time_t>(epochSeconds);
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) == 0) {
        timeValid_ = true;
    }
}

bool TimeKeeper::applyTimezoneRule(const String &tzRule, bool persist) {
  if (tzRule.isEmpty()) {
    return false;
  }

  setenv("TZ", tzRule.c_str(), 1);
  tzset();

  // Persist the fixed-offset rule so rebooted timers and log formatting stay aligned
  // with the last known client timezone until the next explicit sync.
  if (persist && prefs_) {
    prefs_->putString("tz", tzRule);
  }
  return true;
}

void TimeKeeper::persistEpoch(uint64_t epochSeconds) {
  if (!prefs_ || epochSeconds < 1700000000ULL) {
    return;
  }
  prefs_->putULong64("last_epoch", epochSeconds);
  lastPersistedEpoch_ = epochSeconds;
}

void TimeKeeper::persistUserEpoch(uint64_t epochSeconds) {
  if (!prefs_ || epochSeconds < 1700000000ULL) {
    return;
  }
  prefs_->putULong64("last_user_epoch", epochSeconds);
  userTimeValid_ = true;
}

bool TimeKeeper::buildTimeStruct(uint64_t epochSeconds, struct tm *out) const {
  if (!out) {
    return false;
  }
  time_t raw = static_cast<time_t>(epochSeconds);
  struct tm temp;
#if defined(ESP32)
  if (!localtime_r(&raw, &temp)) {
    return false;
  }
#else
  temp = *localtime(&raw);
#endif
  *out = temp;
  return true;
}
