#pragma once

#include <Arduino.h>
#include <vector>

// ACCESS CONTROL START
constexpr size_t MAX_USER_ACCOUNTS = 16;
constexpr size_t MAX_MAC_LENGTH = 18;
constexpr size_t MAX_NAME_LENGTH = 32;
// FIX BUG-AUTH: SHA-256 hex requires 64 chars + 1 null terminator = 65 bytes.
// The previous value of 64 forced strncpy(..., MAX_PASSWORD_LENGTH - 1) to
// truncate every stored hash to 63 chars, so authenticateUser could never
// match the freshly-recomputed 64-char hash. See loadUserAccounts() and the
// /api/auth/addUser handler.
constexpr size_t MAX_PASSWORD_LENGTH = 65;

enum class RelayMode : uint8_t { OFF = 0, ON = 1, AUTO = 2 };
enum class RelayState : uint8_t { OFF = 0, ON = 1 };
enum class ControlSource : uint8_t { NONE = 0, PIR = 1, TIMER = 2, MANUAL = 3 };
enum class DayPhase : uint8_t { DAY = 0, NIGHT = 1 };

struct RelayConfig {
  uint8_t relayPin;
  const char *name;
  float ratedPowerWatts;
};

struct PirConfig {
  uint8_t pin;
  uint64_t relayMask;  // bitmask where bitN => relay N
  const char *name;
};

// PIR MAPPING START
constexpr size_t RELAY_MASK_BITS = sizeof(uint64_t) * 8;

constexpr uint64_t relayMaskForRelay(size_t relayIndex) {
  return relayIndex < RELAY_MASK_BITS ? (uint64_t{1} << relayIndex) : 0ULL;
}

inline uint64_t relayMaskForCount(size_t relayCount) {
  if (relayCount >= RELAY_MASK_BITS) {
    return UINT64_MAX;
  }
  return relayCount == 0 ? 0ULL : ((uint64_t{1} << relayCount) - 1ULL);
}

struct PIRMapping {
  uint64_t relayMask;

  bool controlsRelay(size_t relayIndex) const {
    return (relayMask & relayMaskForRelay(relayIndex)) != 0;
  }

  void setRelay(size_t relayIndex, bool enabled) {
    const uint64_t bit = relayMaskForRelay(relayIndex);
    if (enabled) {
      relayMask |= bit;
    } else {
      relayMask &= ~bit;
    }
  }
};
// PIR MAPPING END

struct TimerPlan {
  bool active;
  uint64_t startEpoch;
  uint64_t endEpoch;
  RelayState targetState;
  RelayState previousState;
  RelayMode previousManualMode;
  uint32_t durationMinutes;
  bool restorePending;
};

struct RelayStats {
  uint32_t timerUses;
  uint32_t totalTimerMinutes;
  uint64_t accumulatedOnSeconds;
  uint64_t lastOnEpoch;
  float totalEnergyWh;
  float lastEnergyWh;
};

struct RelayRuntime {
  RelayMode manualMode;
  RelayState appliedState;
  ControlSource appliedSource;
  TimerPlan timer;
  uint64_t autoHoldUntilEpoch;
  float ratedPowerWatts;
  bool ratedPowerLocked;
  bool energyTrackingActive;
  uint64_t energyStartEpoch;
  RelayStats stats;
};

struct PirRuntime {
  bool rawValue;
  bool stableValue;
  uint32_t lastChangeMs;
  uint64_t lastTriggerEpoch;
};

struct SystemRuntime {
  std::vector<RelayRuntime> relays;
  std::vector<PirRuntime> pirs;
  // PIR MAPPING START
  std::vector<PIRMapping> pirMap;
  // PIR MAPPING END
  bool energyTrackingEnabled;
  uint16_t connectedClients;
  DayPhase dayPhase;
  bool timeValid;
  bool nightLockActive;
};

struct UserAccount {
  char macAddress[MAX_MAC_LENGTH];    // "AA:BB:CC:DD:EE:FF"
  char displayName[MAX_NAME_LENGTH];  // Friendly name
  char passwordHash[MAX_PASSWORD_LENGTH]; // SHA256 hex string
  bool isAdmin;
  bool canManageUsers;
  // RESTRICTED MODE: when true, this user is always redirected to the
  // restricted page (relay+timer only) and cannot access the full dashboard.
  bool restricted;
  uint64_t createdAt;
  uint64_t lastAccess;
};

struct AccessControlRuntime {
  UserAccount users[MAX_USER_ACCOUNTS];
  uint8_t userCount;
  bool enabled;
};
// ACCESS CONTROL END
