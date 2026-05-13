#include "WebPortal.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <cstring>
#include <esp_idf_version.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <time.h>
#include <vector>

#include "Utils.h"

namespace
{
  constexpr size_t MAX_HTTP_BODY_BYTES = 512;
  constexpr size_t MAX_WS_PAYLOAD_BYTES = 384;
  constexpr char FIRMWARE_HMAC_KEY[] = "ESP32_SMARTHOME_FIRMWARE_HMAC_V1";
  constexpr const char *SECURITY_HEADER_SIGNATURE = "X-Firmware-Signature";

  bool containsBlockedInputTokens(const String &value)
  {
    if (value.indexOf("&&") >= 0 || value.indexOf("||") >= 0 || value.indexOf(';') >= 0 || value.indexOf('`') >= 0)
    {
      return true;
    }
    for (size_t i = 0; i < value.length(); ++i)
    {
      const char c = value[i];
      if (c == '\r' || c == '\n' || c == '\0')
      {
        return true;
      }
    }
    return false;
  }

  bool isStrictUnsignedString(const String &value)
  {
    if (value.isEmpty())
    {
      return false;
    }
    for (size_t i = 0; i < value.length(); ++i)
    {
      if (!isDigit(static_cast<unsigned char>(value[i])))
      {
        return false;
      }
    }
    return true;
  }

  bool isAllowedRelayModeValue(const String &value)
  {
    return value == "ON" || value == "OFF" || value == "AUTO";
  }

  bool isAllowedRelayStateValue(const String &value)
  {
    return value == "ON" || value == "OFF";
  }

  bool hasOnlyAllowedKeys(const JsonDocument &doc, std::initializer_list<const char *> allowedKeys)
  {
    JsonObjectConst object = doc.as<JsonObjectConst>();
    if (object.isNull())
    {
      return false;
    }

    for (JsonPairConst pair : object)
    {
      const String key = pair.key().c_str();
      if (containsBlockedInputTokens(key))
      {
        return false;
      }
      bool allowed = false;
      for (const char *allowedKey : allowedKeys)
      {
        if (key == allowedKey)
        {
          allowed = true;
          break;
        }
      }
      if (!allowed)
      {
        return false;
      }
    }
    return true;
  }

  bool hasOnlyAllowedArgs(WebServer &server, std::initializer_list<const char *> allowedArgs)
  {
    for (uint8_t i = 0; i < server.args(); ++i)
    {
      const String name = server.argName(i);
      if (name == "plain")
      {
        continue;
      }
      bool allowed = false;
      for (const char *allowedArg : allowedArgs)
      {
        if (name == allowedArg)
        {
          allowed = true;
          break;
        }
      }
      if (!allowed || containsBlockedInputTokens(name) || containsBlockedInputTokens(server.arg(i)))
      {
        return false;
      }
    }
    return true;
  }

  bool validatePirMappingsJson(const JsonDocument &doc)
  {
    if (!hasOnlyAllowedKeys(doc, {"mappings"}))
    {
      return false;
    }
    JsonArrayConst mappings = doc["mappings"].as<JsonArrayConst>();
    if (mappings.isNull() || mappings.size() != PIR_COUNT)
    {
      return false;
    }
    for (JsonVariantConst itemVar : mappings)
    {
      JsonObjectConst item = itemVar.as<JsonObjectConst>();
      if (item.isNull())
      {
        return false;
      }
      for (JsonPairConst pair : item)
      {
        const String key = pair.key().c_str();
        if (key == "relays")
        {
          JsonArrayConst relayItems = pair.value().as<JsonArrayConst>();
          if (relayItems.isNull() || relayItems.size() != RELAY_COUNT)
          {
            return false;
          }
          for (JsonVariantConst relayValue : relayItems)
          {
            if (!relayValue.is<bool>())
            {
              return false;
            }
          }
          continue;
        }
        if ((key == "relayA" || key == "relayB") && pair.value().is<bool>())
        {
          continue;
        }
        if (key == "relayMask" && (pair.value().is<uint64_t>() || pair.value().is<uint32_t>()))
        {
          continue;
        }
        return false;
      }
      if (!item["relays"].isNull())
      {
        JsonArrayConst relayItems = item["relays"].as<JsonArrayConst>();
        if (relayItems.isNull() || relayItems.size() != RELAY_COUNT)
        {
          return false;
        }
      }
      else if (item["relayMask"].isNull() && !item["relayA"].is<bool>() && !item["relayB"].is<bool>())
      {
        return false;
      }
    }
    return true;
  }

  bool isAllowedWsType(const String &type)
  {
    return type == "time_sync" || type == "set_manual" || type == "set_timer" || type == "cancel_timer" ||
           type == "set_energy_tracking" || type == "get_state";
  }

  bool isOtaPath(const String &uri)
  {
    String lowered = uri;
    lowered.toLowerCase();
    return lowered.indexOf("/ota") >= 0 || lowered.indexOf("update") >= 0 || lowered.indexOf("firmware") >= 0;
  }

  bool decodeHexNibble(char c, uint8_t *value)
  {
    if (!value)
    {
      return false;
    }
    if (c >= '0' && c <= '9')
    {
      *value = static_cast<uint8_t>(c - '0');
      return true;
    }
    if (c >= 'a' && c <= 'f')
    {
      *value = static_cast<uint8_t>(10 + (c - 'a'));
      return true;
    }
    if (c >= 'A' && c <= 'F')
    {
      *value = static_cast<uint8_t>(10 + (c - 'A'));
      return true;
    }
    return false;
  }

  bool decodeHexString(const String &hex, uint8_t *out, size_t outLen)
  {
    if (!out || hex.length() != outLen * 2)
    {
      return false;
    }
    for (size_t i = 0; i < outLen; ++i)
    {
      uint8_t high = 0;
      uint8_t low = 0;
      if (!decodeHexNibble(hex[i * 2], &high) || !decodeHexNibble(hex[i * 2 + 1], &low))
      {
        return false;
      }
      out[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
  }

  bool constantTimeEquals(const uint8_t *lhs, const uint8_t *rhs, size_t len)
  {
    if (!lhs || !rhs)
    {
      return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i)
    {
      diff |= lhs[i] ^ rhs[i];
    }
    return diff == 0;
  }

  bool verifyFirmwareSignature(const uint8_t *imageData, size_t imageSize, const String &signatureHex)
  {
    constexpr size_t HMAC_SIZE = 32;
    if (!imageData || imageSize == 0 || containsBlockedInputTokens(signatureHex))
    {
      return false;
    }

    uint8_t expected[HMAC_SIZE];
    if (!decodeHexString(signatureHex, expected, sizeof(expected)))
    {
      return false;
    }

    const mbedtls_md_info_t *mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!mdInfo)
    {
      return false;
    }

    uint8_t computed[HMAC_SIZE];
    const int result = mbedtls_md_hmac(mdInfo,
                                       reinterpret_cast<const unsigned char *>(FIRMWARE_HMAC_KEY),
                                       strlen(FIRMWARE_HMAC_KEY),
                                       reinterpret_cast<const unsigned char *>(imageData),
                                       imageSize,
                                       computed);
    return result == 0 && constantTimeEquals(expected, computed, sizeof(computed));
  }

  bool eventNeedsSnapshot(const String &jsonLine)
  {
    JsonDocument doc;
    if (deserializeJson(doc, jsonLine))
    {
      return false;
    }
    const String event = doc["event"] | "";
    return event == "relay.changed" || event == "timer.started" || event == "timer.ended" || event == "timer.canceled" ||
           event == "manual.changed" || event == "mode.changed" ||
           event == "client.connected" || event == "client.disconnected" ||
           event == "night_lock.activated" || event == "night_lock.released" || event == "relay.night_forced_off" ||
           event == "energy_tracking.changed";
  }

  bool shouldPersistEventToFlash(const String &jsonLine)
  {
    JsonDocument doc;
    if (deserializeJson(doc, jsonLine))
    {
      return true;
    }

    const String event = doc["event"] | "";
    // STORAGE MANAGEMENT: exclude all PIR-sensor events from flash so the
    // activity log only grows when relay state actually changes.
    return event != "pir.motion" && event != "pir.idle" && event != "pir.blocked";
  }

  bool shouldRateLimitRelayCommand(size_t relayIndex)
  {
    constexpr uint32_t RELAY_COMMAND_RATE_LIMIT_MS = 120UL;
    static std::vector<uint32_t> lastRelayCommandMs;
    if (lastRelayCommandMs.size() != RELAY_COUNT)
    {
      lastRelayCommandMs.assign(RELAY_COUNT, 0);
    }

    if (relayIndex >= RELAY_COUNT)
    {
      return false;
    }

    const uint32_t nowMs = millis();
    if (lastRelayCommandMs[relayIndex] != 0 &&
        static_cast<uint32_t>(nowMs - lastRelayCommandMs[relayIndex]) < RELAY_COMMAND_RATE_LIMIT_MS)
    {
      return true;
    }

    lastRelayCommandMs[relayIndex] = nowMs;
    return false;
  }
} // namespace

WebPortal *WebPortal::instance_ = nullptr;

WebPortal::CommandContextGuard::~CommandContextGuard()
{
  if (portal_)
  {
    portal_->clearCommandContext();
  }
}

void WebPortal::begin(ControlEngine *engine, StorageLayer *storage, TimeKeeper *timeKeeper)
{
  engine_ = engine;
  storage_ = storage;
  timeKeeper_ = timeKeeper;

  if (!initialized_)
  {
    outboundQueue_ = xQueueCreate(48, sizeof(QueuedEvent));
    inboundQueue_ = xQueueCreate(24, sizeof(QueuedCommand));
    contextMutex_ = xSemaphoreCreateMutex();

    const char *collectedHeaders[] = {SECURITY_HEADER_SIGNATURE};
    server_.collectHeaders(collectedHeaders, 1);
    setupRoutes();
    initialized_ = true;
  }

  if (running_)
  {
    return;
  }

  clients_.fill(false);
  clientMacs_.fill("");
  connectedClients_ = 0;
  lastClientRefreshMs_ = 0;
  stateBroadcastPending_ = false;
  captivePortalEnabled_ = false;
  captivePortalIp_ = IPAddress(0, 0, 0, 0);

  instance_ = this;
  server_.begin();
  socket_.begin();

  // ACCESS CONTROL - Load user accounts from storage
  storage_->loadUserAccounts(&accessControl_);

  socket_.enableHeartbeat(15000, 3500, 2);
  socket_.onEvent(onWsEventStatic);
  beginCaptivePortal();
  running_ = true;
}

void WebPortal::end()
{
  if (!initialized_ || !running_)
  {
    return;
  }

  // ONLINE mode must not expose any offline HTTP/WebSocket/MAC-auth surface.
  // Stop listeners in-place and keep queues allocated to avoid heap churn if
  // the device later falls back to offline AP mode.
  dnsServer_.stop();
  captivePortalEnabled_ = false;
  captivePortalIp_ = IPAddress(0, 0, 0, 0);
  for (uint8_t i = 0; i < WS_MAX_CLIENTS; ++i)
  {
    socket_.disconnect(i);
  }
  socket_.close();
  server_.stop();

  clients_.fill(false);
  clientMacs_.fill("");
  connectedClients_ = 0;
  stateBroadcastPending_ = false;
  updateClientCountInEngine();
  running_ = false;
}

void WebPortal::recoverAfterAccessPointRestart()
{
  if (!running_)
  {
    return;
  }
  clients_.fill(false);
  clientMacs_.fill("");
  connectedClients_ = 0;
  stateBroadcastPending_ = false;
  updateClientCountInEngine();

  server_.begin();
  socket_.begin();
  socket_.onEvent(onWsEventStatic);
  beginCaptivePortal();
}

void WebPortal::loop()
{
  if (!running_)
  {
    return;
  }
  ensureCaptivePortal();
  if (captivePortalEnabled_)
  {
    dnsServer_.processNextRequest();
  }
  server_.handleClient();
  socket_.loop();
  if (millis() - lastClientRefreshMs_ >= 1000UL)
  {
    lastClientRefreshMs_ = millis();
    syncConnectedClients(true);
  }
  processInboundCommands();
  processQueue();
  processPendingStateBroadcast();
}

bool WebPortal::isRunning() const { return running_; }

bool WebPortal::enqueueEvent(const String &eventJson, bool bufferIfOffline)
{
  if (!running_ || !outboundQueue_)
  {
    return false;
  }

  QueuedEvent queued{};
  queued.bufferIfOffline = bufferIfOffline;
  eventJson.substring(0, sizeof(queued.json) - 1).toCharArray(queued.json, sizeof(queued.json));

  String mac = activeCommandMac();
  if (mac.isEmpty())
  {
    mac = "SYSTEM";
  }
  mac.substring(0, sizeof(queued.mac) - 1).toCharArray(queued.mac, sizeof(queued.mac));

  if (xQueueSend(outboundQueue_, &queued, 0) == pdTRUE)
  {
    return true;
  }

  QueuedEvent dropped{};
  xQueueReceive(outboundQueue_, &dropped, 0);
  return xQueueSend(outboundQueue_, &queued, 0) == pdTRUE;
}

uint16_t WebPortal::connectedClientCount() const { return connectedClients_; }

void WebPortal::setupRoutes()
{
  // ============================================================
  // ACCESS CONTROL ENDPOINTS
  // ============================================================
  server_.on("/api/auth/status", HTTP_GET, [this]()
             {
    String mac = macFromHttpClient();
    JsonDocument doc;
    const bool authed = validateMacAccess(mac);
    doc["authenticated"] = authed;
    doc["mac"] = mac;
    // FIX UI-RBAC: expose role flags for the current MAC so the frontend can
    // apply the role-based visibility matrix without calling /api/auth/users
    // (which is gated behind manageUsers and would 403 for ordinary users).
    bool isAdminFlag = false;
    bool canManageFlag = false;
    bool isRestrictedFlag = false;
    if (authed) {
      UserAccount *u = storage_->findUserByMac(mac.c_str());
      if (u) {
        isAdminFlag = u->isAdmin;
        canManageFlag = u->canManageUsers;
        isRestrictedFlag = u->restricted;
      }
    }
    doc["isAdmin"] = isAdminFlag;
    doc["canManageUsers"] = canManageFlag;
    doc["isRestricted"] = isRestrictedFlag;
    String payload;
    serializeJson(doc, payload);
    server_.send(200, "application/json", payload); });

  server_.on("/api/auth/login", HTTP_POST, [this]()
             {
    const String body = server_.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      server_.send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
      return;
    }

    String mac = doc["mac"] | "";
    String password = doc["password"] | "";

    if (!storage_->validateMacFormat(mac.c_str())) {
      server_.send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid MAC format\"}");
      return;
    }

    if (authenticateUser(mac, password)) {
      updateLastAccess(mac.c_str());
      server_.send(200, "application/json", "{\"ok\":true,\"msg\":\"Authenticated\"}");
    } else {
      server_.send(401, "application/json", "{\"ok\":false,\"msg\":\"Invalid credentials\"}");
    } });

  server_.on("/api/auth/addUser", HTTP_POST, [this]()
             {
    String mac = macFromHttpClient();
    Serial.printf("[AddUser] Request from MAC: %s\n", mac.c_str());
    UserAccount *requester = storage_->findUserByMac(mac.c_str());
    if (!requester) {
    Serial.println("[AddUser] Requester not found");
} else {
    Serial.printf("[AddUser] Requester found: %s, canManage=%d\n",
        requester->macAddress,
        requester->canManageUsers);
}
    if (!requester || !checkPermission(*requester, "manageUsers")) {
      Serial.println("[AddUser] Permission denied (requester null or not manager)");
      server_.send(403, "application/json", "{\"ok\":false,\"msg\":\"Insufficient permissions\"}");
      return;
    }
Serial.printf("[AddUser] Requester OK: %s\n", requester->macAddress);
    const String body = server_.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      Serial.println("[AddUser] Invalid JSON body");
      server_.send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
      return;
    }

    String adminPassword = doc["adminPassword"] | "";
    if (!authenticateUser(mac, adminPassword)) {
      Serial.println("[AddUser] Admin password incorrect");
      server_.send(401, "application/json", "{\"ok\":false,\"msg\":\"Admin password incorrect\"}");
      return;
    }

    UserAccount newUser;
    memset(&newUser, 0, sizeof(newUser));

    String newMac = doc["newMac"] | "";
    String newName = doc["newName"] | "";
    String newPassword = doc["newPassword"] | "";

    if (!storage_->validateMacFormat(newMac.c_str())) {
      server_.send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid MAC format\"}");
      return;
    }

    if (newName.isEmpty() || newPassword.isEmpty()) {
      server_.send(400, "application/json", "{\"ok\":false,\"msg\":\"Name and password required\"}");
      return;
    }

    String upperMac = newMac;
    upperMac.toUpperCase();

    // FIX: explicit duplicate check BEFORE we attempt to persist so the
    // frontend can show "Already exists" instead of the generic
    // "Failed to add user". StorageLayer::addUserAccount (line 955) also
    // rejects duplicates but returns a single boolean for every failure
    // mode, which is ambiguous to the UI.
    for (uint8_t i = 0; i < accessControl_.userCount; i++) {
      if (strcmp(accessControl_.users[i].macAddress, upperMac.c_str()) == 0) {
        Serial.printf("[AddUser] Duplicate MAC rejected: %s\n", upperMac.c_str());
        server_.send(409, "application/json",
                     "{\"ok\":false,\"msg\":\"Already exists\"}");
        return;
      }
    }

    strncpy(newUser.macAddress, upperMac.c_str(), MAX_MAC_LENGTH - 1);
    newUser.macAddress[MAX_MAC_LENGTH - 1] = '\0';
    strncpy(newUser.displayName, newName.c_str(), MAX_NAME_LENGTH - 1);
    newUser.displayName[MAX_NAME_LENGTH - 1] = '\0';

    uint8_t hash[32];
    mbedtls_sha256_ret(reinterpret_cast<const unsigned char *>(newPassword.c_str()),
                       newPassword.length(), hash, 0);
    char hexString[65];
    for (size_t i = 0; i < 32; i++) {
      snprintf(hexString + (i * 2), 3, "%02x", hash[i]);
    }
    hexString[64] = '\0';
    // FIX BUG-AUTH: copy all 64 hex chars and force null terminator at
    // the last buffer byte (MAX_PASSWORD_LENGTH is now 65).
    strncpy(newUser.passwordHash, hexString, MAX_PASSWORD_LENGTH - 1);
    newUser.passwordHash[MAX_PASSWORD_LENGTH - 1] = '\0';

    newUser.isAdmin = doc["isAdmin"] | false;
    newUser.canManageUsers = doc["canManageUsers"] | false;
    // RESTRICTED MODE: persist the restricted flag set by the admin.
    newUser.restricted = doc["isRestricted"] | false;
    newUser.createdAt = time(nullptr);

    // STORAGE MANAGEMENT: pre-flight check. Detect a full roster BEFORE
    // attempting any write so the frontend receives a structured error that
    // lets the user choose a recovery option.
    if (accessControl_.userCount >= MAX_USER_ACCOUNTS) {
      JsonDocument sfDoc;
      sfDoc["ok"] = false;
      sfDoc["error"] = "storage_full";
      JsonArray opts = sfDoc["options"].to<JsonArray>();
      opts.add("disable_logs");
      opts.add("remove_inactive_users");
      sfDoc["msg"] = String("User roster full (") + MAX_USER_ACCOUNTS +
                      "/" + MAX_USER_ACCOUNTS +
                      "). Free space first.";
      String sfPayload;
      serializeJson(sfDoc, sfPayload);
      server_.send(507, "application/json", sfPayload);
      return;
    }

    if (storage_->addUserAccount(newUser)) {
      // FIX RC-1: keep the in-memory access cache in sync with persisted NVS.
      // Without this, validateMacAccess() and /api/auth/users keep serving the
      // snapshot loaded at boot, so the new user is invisible until reboot.
      storage_->loadUserAccounts(&accessControl_);
      server_.send(200, "application/json",
                   "{\"ok\":true,\"msg\":\"User added successfully\"}");
    } else {
      // Reached only when the storage layer fails for non-duplicate reasons
      // (NVS lock contention, write failure).
      server_.send(500, "application/json",
                   "{\"ok\":false,\"msg\":\"Storage write failed (locked)\"}");
    } });

  server_.on("/api/auth/users", HTTP_GET, [this]()
             {
    String mac = macFromHttpClient();
    UserAccount *requester = storage_->findUserByMac(mac.c_str());
    if (!requester) {
      Serial.println("[Users] Requester not found");
    } else {
      Serial.printf("[Users] Requester found: %s, canManage=%d\n",
                    requester->macAddress, requester->canManageUsers);
    }
    if (!requester || !checkPermission(*requester, "manageUsers")) {
      server_.send(403, "application/json",
                   "{\"ok\":false,\"msg\":\"Insufficient permissions\"}");
      return;
    }

    // FIX RC-1: refresh the cache before listing so out-of-band edits or a
    // previous failed sync cannot leave the UI showing a stale roster.
    storage_->loadUserAccounts(&accessControl_);

    JsonDocument doc;
    doc["ok"] = true;                 // FIX RC-2: frontend's loadUserList() requires data.ok.
    JsonArray usersArray = doc["users"].to<JsonArray>();

    for (uint8_t i = 0; i < accessControl_.userCount; i++) {
      JsonObject userObj = usersArray.add<JsonObject>();
      userObj["mac"] = accessControl_.users[i].macAddress;
      userObj["name"] = accessControl_.users[i].displayName;
      userObj["isAdmin"] = accessControl_.users[i].isAdmin;
      userObj["canManageUsers"] = accessControl_.users[i].canManageUsers;
      userObj["isRestricted"] = accessControl_.users[i].restricted;
      userObj["lastAccess"] = accessControl_.users[i].lastAccess;
    }

    String payload;
    serializeJson(doc, payload);
    server_.send(200, "application/json", payload); });

  server_.on("/api/auth/removeUser", HTTP_POST, [this]()
             {
    String mac = macFromHttpClient();
    UserAccount *requester = storage_->findUserByMac(mac.c_str());
    if (!requester) {
      Serial.println("[RemoveUser] Requester not found");
    } else {
      Serial.printf("[RemoveUser] Requester found: %s, canManage=%d\n",
                    requester->macAddress, requester->canManageUsers);
    }
    // FIX RC-4: align permission policy with /api/auth/addUser. Previously this
    // endpoint required isAdmin while addUser accepted canManageUsers, which
    // meant a "manager" could add users but could never delete them.
    if (!requester || !checkPermission(*requester, "manageUsers")) {
      server_.send(403, "application/json",
                   "{\"ok\":false,\"msg\":\"Insufficient permissions\"}");
      return;
    }

    const String body = server_.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      server_.send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
      return;
    }

    // FIX RC-3: re-authenticate with the admin password the UI prompts for.
    // The previous handler ignored adminPassword entirely, so anyone whose MAC
    // matched an admin entry could delete users without proving identity.
    String adminPassword = doc["adminPassword"] | "";
    if (!authenticateUser(mac, adminPassword)) {
      server_.send(401, "application/json",
                   "{\"ok\":false,\"msg\":\"Admin password incorrect\"}");
      return;
    }

    String targetMac = doc["mac"] | "";
    if (!storage_->validateMacFormat(targetMac.c_str())) {
      server_.send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid MAC\"}");
      return;
    }

    // Normalize like StorageLayer does (NVS stores MACs upper-cased), otherwise
    // the self-removal guard below could be bypassed by simple case mixing.
    String targetUpper = targetMac;
    targetUpper.toUpperCase();
    if (strcmp(targetUpper.c_str(), mac.c_str()) == 0) {
      server_.send(400, "application/json",
                   "{\"ok\":false,\"msg\":\"Cannot remove yourself\"}");
      return;
    }

    if (storage_->removeUserAccount(targetUpper.c_str())) {
      // FIX RC-1: refresh cache so the removed user can no longer authorize.
      storage_->loadUserAccounts(&accessControl_);
      server_.send(200, "application/json", "{\"ok\":true,\"msg\":\"User removed\"}");
    } else {
      server_.send(400, "application/json", "{\"ok\":false,\"msg\":\"Failed to remove\"}");
    } });

  // ---- STORAGE MANAGEMENT: disable activity logging ----------------------
  server_.on("/api/auth/disableLogs", HTTP_POST, [this]()
             {
    String mac = macFromHttpClient();
    UserAccount *requester = storage_->findUserByMac(mac.c_str());
    if (!requester || !checkPermission(*requester, "admin")) {
      server_.send(403, "application/json",
                   "{\"ok\":false,\"msg\":\"Insufficient permissions\"}");
      return;
    }
    const String body = server_.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      server_.send(400, "application/json",
                   "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
      return;
    }
    String adminPassword = doc["adminPassword"] | "";
    if (!authenticateUser(mac, adminPassword)) {
      server_.send(401, "application/json",
                   "{\"ok\":false,\"msg\":\"Admin password incorrect\"}");
      return;
    }
    storage_->setLogsEnabled(false);
    // Remove existing log file to immediately reclaim flash space.
    storage_->clearLogFiles();
    server_.send(200, "application/json",
                 "{\"ok\":true,\"msg\":\"Activity logging disabled. Log file cleared.\"}");
  });

  // ---- STORAGE MANAGEMENT: remove inactive users -------------------------
  server_.on("/api/auth/removeInactiveUsers", HTTP_POST, [this]()
             {
    String mac = macFromHttpClient();
    UserAccount *requester = storage_->findUserByMac(mac.c_str());
    if (!requester || !checkPermission(*requester, "admin")) {
      server_.send(403, "application/json",
                   "{\"ok\":false,\"msg\":\"Insufficient permissions\"}");
      return;
    }
    const String body = server_.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      server_.send(400, "application/json",
                   "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
      return;
    }
    String adminPassword = doc["adminPassword"] | "";
    if (!authenticateUser(mac, adminPassword)) {
      server_.send(401, "application/json",
                   "{\"ok\":false,\"msg\":\"Admin password incorrect\"}");
      return;
    }
    const uint64_t nowEpoch =
        timeKeeper_->nowUserEpoch() > 0 ? timeKeeper_->nowUserEpoch()
                                        : static_cast<uint64_t>(time(nullptr));
    const uint8_t removed =
        storage_->removeInactiveUsers(nowEpoch, LOG_INACTIVITY_DAYS, true);
    storage_->loadUserAccounts(&accessControl_);

    if (removed == 0 && storage_->isUserStorageFull()) {
      // Nothing was removed but roster is still full: all non-admin accounts
      // have been active recently. Manual intervention is required.
      server_.send(507, "application/json",
                   "{\"ok\":false,\"error\":\"manual_cleanup_required\","
                   "\"msg\":\"Please remove inactive users manually\"}");
      return;
    }

    JsonDocument resp;
    resp["ok"] = true;
    resp["removed"] = removed;
    resp["msg"] = String(removed) + " inactive user(s) removed.";
    String respPayload;
    serializeJson(resp, respPayload);
    server_.send(200, "application/json", respPayload);
  });
  // ============================================================

  // CAPTIVE PORTAL START
  auto captiveProbeHandler = [this]()
  { sendCaptivePortalResponse(200); };
  server_.on("/fwlink", HTTP_ANY, captiveProbeHandler);
  server_.on("/generate_204", HTTP_ANY, captiveProbeHandler);
  server_.on("/gen_204", HTTP_ANY, captiveProbeHandler);
  server_.on("/hotspot-detect.html", HTTP_ANY, captiveProbeHandler);
  server_.on("/connecttest.txt", HTTP_ANY, captiveProbeHandler);
  server_.on("/library/test/success.html", HTTP_ANY, captiveProbeHandler);
  server_.on("/success.txt", HTTP_ANY, captiveProbeHandler);
  server_.on("/canonical.html", HTTP_ANY, captiveProbeHandler);
  server_.on("/redirect", HTTP_ANY, captiveProbeHandler);
  server_.on("/ncsi.txt", HTTP_ANY, captiveProbeHandler);
  // CAPTIVE PORTAL END

  auto otaDisabledHandler = [this]()
  {
    if (server_.hasHeader(SECURITY_HEADER_SIGNATURE))
    {
      const String body = server_.arg("plain");
      if (!body.isEmpty() && body.length() <= MAX_HTTP_BODY_BYTES)
      {
        (void)verifyFirmwareSignature(reinterpret_cast<const uint8_t *>(body.c_str()),
                                      body.length(),
                                      server_.header(SECURITY_HEADER_SIGNATURE));
      }
    }
    server_.send(403, "application/json", "{\"ok\":false,\"msg\":\"OTA disabled\"}");
  };
  server_.on("/update", HTTP_ANY, otaDisabledHandler);
  server_.on("/ota", HTTP_ANY, otaDisabledHandler);
  server_.on("/api/update", HTTP_ANY, otaDisabledHandler);
  server_.on("/firmware", HTTP_ANY, otaDisabledHandler);

  server_.on("/", HTTP_GET, [this]()
             {
    if (shouldRedirectToCaptivePortal()) {
      sendCaptivePortalResponse(302);
      return;
    }
    // RESTRICTED MODE: if the connecting client's MAC is registered as
    // restricted, serve the restricted relay+timer page instead of the full
    // dashboard. Non-authenticated clients are not redirected here because
    // they will be caught by the onNotFound gate on any protected route.
    if (ENABLE_ACCESS_CONTROL) {
      String mac = macFromHttpClient();
      if (validateMacAccess(mac)) {
        UserAccount *u = storage_->findUserByMac(mac.c_str());
        if (u && u->restricted) {
          server_.sendHeader("Location", "/restricted.html", true);
          server_.sendHeader("Cache-Control", "no-cache", true);
          server_.send(302, "text/plain", "");
          return;
        }
      }
    }
    File file = LittleFS.open("/index.html", FILE_READ);
    if (!file) {
      server_.send(500, "text/plain", "Missing /index.html on LittleFS.");
      return;
    }
    server_.streamFile(file, "text/html");
    file.close(); });

  server_.on("/api/state", HTTP_GET, [this]()
             {
    if (!hasOnlyAllowedArgs(server_, {})) {
      server_.send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid query\"}");
      return;
    }
    server_.send(200, "application/json", engine_->buildStateJson()); });

  server_.on("/api/logs", HTTP_GET, [this]()
             {
    if (!hasOnlyAllowedArgs(server_, {"limit"}) ||
        (server_.hasArg("limit") && !isStrictUnsignedString(server_.arg("limit")))) {
      server_.send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid query\"}");
      return;
    }
    const uint16_t limit = static_cast<uint16_t>(server_.hasArg("limit") ? server_.arg("limit").toInt() : 80);
    server_.send(200, "application/json", storage_->readRecentLogsJson(limit)); });

  server_.on("/api/time", HTTP_GET, [this]()
             {
    if (!hasOnlyAllowedArgs(server_, {})) {
      server_.send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid query\"}");
      return;
    }
    JsonDocument doc;
    const uint64_t apiEpoch = timeKeeper_->nowUserEpoch() > 0 ? timeKeeper_->nowUserEpoch() : timeKeeper_->nowEpoch();
    doc["epoch"] = apiEpoch;
    doc["valid"] = timeKeeper_->hasValidTime();
    doc["userValid"] = timeKeeper_->hasUserTime();
    doc["dayPhase"] = dayPhaseToText(timeKeeper_->currentDayPhase());
    String payload;
    serializeJson(doc, payload);
    server_.send(200, "application/json", payload); });

  // PIR MAPPING START
  server_.on("/api/pirMapping", HTTP_POST, [this]()
             {
    JsonDocument response;
    response["ok"] = false;
    const String body = server_.arg("plain");
    if (body.isEmpty() || body.length() > MAX_HTTP_BODY_BYTES || containsBlockedInputTokens(body)) {
      response["msg"] = "Invalid JSON body.";
      String payload;
      serializeJson(response, payload);
      server_.send(400, "application/json", payload);
      return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body) || !validatePirMappingsJson(doc)) {
      response["msg"] = "Invalid JSON body.";
      String payload;
      serializeJson(response, payload);
      server_.send(400, "application/json", payload);
      return;
    }

    JsonArray mappingsJson = doc["mappings"].as<JsonArray>();
    if (mappingsJson.isNull() || mappingsJson.size() != PIR_COUNT) {
      response["msg"] = "Expected one mapping entry per PIR.";
      String payload;
      serializeJson(response, payload);
      server_.send(400, "application/json", payload);
      return;
    }

    std::vector<PIRMapping> mappings(PIR_COUNT);
    for (size_t i = 0; i < PIR_COUNT; ++i) {
      JsonObject item = mappingsJson[i];
      uint64_t relayMask = 0;
      if (!item["relays"].isNull()) {
        JsonArray relays = item["relays"].as<JsonArray>();
        for (size_t relayIndex = 0; relayIndex < RELAY_COUNT; ++relayIndex) {
          if (relays[relayIndex] | false) {
            relayMask |= relayMaskForRelay(relayIndex);
          }
        }
      } else if (!item["relayMask"].isNull()) {
        relayMask = item["relayMask"].as<uint64_t>();
      } else {
        if (RELAY_COUNT > 0 && (item["relayA"] | false)) {
          relayMask |= relayMaskForRelay(0);
        }
        if (RELAY_COUNT > 1 && (item["relayB"] | false)) {
          relayMask |= relayMaskForRelay(1);
        }
      }
      mappings[i].relayMask = relayMask & relayMaskForCount(RELAY_COUNT);
    }

    String errorText;
    const bool ok = engine_->setPirMapping(mappings, &errorText);
    response["ok"] = ok;
    response["msg"] = ok ? "Sensor mapping saved." : errorText;
    String payload;
    serializeJson(response, payload);
    server_.send(ok ? 200 : 400, "application/json", payload);
    if (ok) {
      scheduleStateBroadcast();
    } });
  // PIR MAPPING END

  // POWER RESET START
  server_.on("/api/resetConsumption", HTTP_POST, [this]()
             {
    if (!hasOnlyAllowedArgs(server_, {}) || !server_.arg("plain").isEmpty()) {
      server_.send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid request\"}");
      return;
    }
    String errorText;
    const bool ok = engine_->resetConsumption(&errorText);

    JsonDocument doc;
    doc["ok"] = ok;
    doc["msg"] = ok ? "Consumption counters reset." : errorText;
    String payload;
    serializeJson(doc, payload);
    server_.send(ok ? 200 : 400, "application/json", payload); });
  // POWER RESET END

  // RATED DYNAMIC START
  server_.on("/api/ratedPower", HTTP_POST, [this]()
             {
    JsonDocument response;
    response["ok"] = false;
    const String body = server_.arg("plain");
    if (body.isEmpty() || body.length() > MAX_HTTP_BODY_BYTES || containsBlockedInputTokens(body)) {
      response["msg"] = "Invalid JSON body.";
      String payload;
      serializeJson(response, payload);
      server_.send(400, "application/json", payload);
      return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body) || !hasOnlyAllowedKeys(doc, {"channel", "powerW"})) {
      response["msg"] = "Invalid JSON body.";
      String payload;
      serializeJson(response, payload);
      server_.send(400, "application/json", payload);
      return;
    }

    const size_t channel = static_cast<size_t>(doc["channel"] | static_cast<int>(RELAY_COUNT));
    const float powerW = doc["powerW"] | 0.0f;
    String errorText;
    const bool ok = engine_->setRatedPower(channel, powerW, &errorText);

    response["ok"] = ok;
    response["msg"] = ok ? "Rated power saved." : errorText;
    String payload;
    serializeJson(response, payload);
    server_.send(ok ? 200 : 400, "application/json", payload); });
  // RATED DYNAMIC END

  auto setTimeHandler = [this]()
  {
    const String body = server_.arg("plain");
    if (body.isEmpty() || body.length() > MAX_HTTP_BODY_BYTES || containsBlockedInputTokens(body))
    {
      server_.send(400, "application/json", "{\"type\":\"set_time_ack\",\"ok\":false,\"status\":\"ERROR\",\"msg\":\"Invalid JSON body.\"}");
      return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body) ||
        !hasOnlyAllowedKeys(doc, {"epoch", "year", "month", "day", "hours", "minutes", "seconds", "tzOffsetMinutes"}))
    {
      server_.send(400, "application/json", "{\"type\":\"set_time_ack\",\"ok\":false,\"status\":\"ERROR\",\"msg\":\"Invalid JSON body.\"}");
      return;
    }

    const String payload = handleSetTime(body);
    const int status = payload.indexOf("\"ok\":true") >= 0 ? 200 : 400;
    server_.send(status, "application/json", payload);
  };
  server_.on("/setTime", HTTP_POST, setTimeHandler);
  server_.on("/api/setTime", HTTP_POST, setTimeHandler);
  server_.on("/unauthorized.html", HTTP_GET, [this]()
             {
  File file = LittleFS.open(FILE_UNAUTHORIZED, FILE_READ);
  if (!file) {
    server_.send(500, "text/plain", "Missing unauthorized page.");
    return;
  }
  server_.streamFile(file, "text/html");
  file.close(); });
  // RESTRICTED MODE: serve the restricted relay+timer page.
  // Authenticated-but-restricted users reach this via the / redirect.
  // The page itself also validates auth via /api/auth/status on load.
  server_.on("/restricted.html", HTTP_GET, [this]()
             {
  File file = LittleFS.open("/restricted.html", FILE_READ);
  if (!file) {
    server_.send(500, "text/plain", "Missing /restricted.html on LittleFS.");
    return;
  }
  server_.streamFile(file, "text/html");
  file.close(); });
  server_.onNotFound([this]()
                     {
    // ACCESS CONTROL - Check authorization before serving any route
    String mac = macFromHttpClient();
    if (ENABLE_ACCESS_CONTROL && !isAuthorizedRoute(server_.uri()) && !validateMacAccess(mac)) {
      File file = LittleFS.open(FILE_UNAUTHORIZED, FILE_READ);
      if (file) {
        server_.sendHeader("Cache-Control", "no-cache");
        server_.streamFile(file, "text/html");
        file.close();
        return;
      }
      server_.send(403, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    if (isOtaPath(server_.uri())) {
      server_.send(403, "application/json", "{\"ok\":false,\"msg\":\"OTA disabled\"}");
      return;
    }
    if (server_.uri() == "/index.html") {
      File file = LittleFS.open("/index.html", FILE_READ);
      if (file) {
        server_.streamFile(file, "text/html");
        file.close();
        return;
      }
    }
    sendCaptivePortalResponse(302); });
}

// CAPTIVE PORTAL START
void WebPortal::beginCaptivePortal()
{
  const IPAddress apIp = WiFi.softAPIP();
  if (apIp == IPAddress(0, 0, 0, 0))
  {
    captivePortalEnabled_ = false;
    captivePortalIp_ = IPAddress(0, 0, 0, 0);
    return;
  }

  dnsServer_.stop();
  dnsServer_.setErrorReplyCode(DNSReplyCode::NoError);
  captivePortalIp_ = apIp;
  captivePortalEnabled_ = dnsServer_.start(53, "*", apIp);
}

void WebPortal::ensureCaptivePortal()
{
  const IPAddress apIp = WiFi.softAPIP();
  if (apIp == IPAddress(0, 0, 0, 0))
  {
    if (captivePortalEnabled_)
    {
      dnsServer_.stop();
      captivePortalEnabled_ = false;
      captivePortalIp_ = IPAddress(0, 0, 0, 0);
    }
    return;
  }

  if (!captivePortalEnabled_ || captivePortalIp_ != apIp)
  {
    beginCaptivePortal();
  }
}

String WebPortal::captivePortalUrl() const
{
  const IPAddress apIp = WiFi.softAPIP();
  if (apIp == IPAddress(0, 0, 0, 0))
  {
    return "http://192.168.4.1/";
  }
  return String("http://") + apIp.toString() + "/";
}

bool WebPortal::shouldRedirectToCaptivePortal()
{
  const String host = server_.hostHeader();
  if (host.isEmpty())
  {
    return false;
  }

  const String apIp = WiFi.softAPIP().toString();
  if (host.equalsIgnoreCase(apIp) || host.equalsIgnoreCase(apIp + ":80"))
  {
    return false;
  }
  if (host.equalsIgnoreCase("localhost") || host.equalsIgnoreCase("esp32") || host.equalsIgnoreCase("esp32.local"))
  {
    return false;
  }
  return true;
}

void WebPortal::sendCaptivePortalResponse(int httpCode)
{
  const String redirectUrl = captivePortalUrl();
  server_.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate", true);
  server_.sendHeader("Pragma", "no-cache", true);
  server_.sendHeader("Expires", "0", true);
  if (httpCode >= 300 && httpCode < 400)
  {
    server_.sendHeader("Location", redirectUrl, true);
  }

  String html;
  html.reserve(320);
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='0; url=";
  html += redirectUrl;
  html += "'>";
  html += "<title>ESP32 Portal</title></head><body>";
  html += "<script>window.location.replace('";
  html += redirectUrl;
  html += "');</script>";
  html += "<p>Opening portal... <a href='";
  html += redirectUrl;
  html += "'>Continue</a></p></body></html>";

  server_.send(httpCode, "text/html", html);
}
// CAPTIVE PORTAL END

void WebPortal::handleWsEvent(uint8_t clientId, WStype_t type, uint8_t *payload, size_t length)
{
  switch (type)
  {
  case WStype_CONNECTED:
  {
    // ACCESS CONTROL - Validate MAC on WebSocket connect
    String mac = getConnectedClientMac(clientId);
    if (ENABLE_ACCESS_CONTROL && !validateMacAccess(mac))
    {
      socket_.disconnect(clientId);
      return;
    }
    onClientConnected(clientId);
    break;
  }
  case WStype_DISCONNECTED:
    onClientDisconnected(clientId);
    break;
  case WStype_TEXT:
  {
    String message;
    message.reserve(length + 1);
    for (size_t i = 0; i < length; ++i)
    {
      message += static_cast<char>(payload[i]);
    }
    enqueueInboundCommand(clientId, message);
    break;
  }
  default:
    break;
  }
}

void WebPortal::onClientConnected(uint8_t clientId)
{
  const String detectedMac = resolveClientMac(clientId);
  String mac = "UNKNOWN";
  if (clientId < clients_.size())
  {
    if (contextMutex_ && xSemaphoreTake(contextMutex_, pdMS_TO_TICKS(50)) == pdTRUE)
    {
      clients_[clientId] = true;
      clientMacs_[clientId] = detectedMac;
      mac = detectedMac;
      xSemaphoreGive(contextMutex_);
    }
    else
    {
      mac = detectedMac.isEmpty() ? "UNKNOWN" : detectedMac;
      // If mutex not taken, we still track the MAC loosely for the event,
      // but do not modify the shared array to avoid inconsistency.
    }
  }
  syncConnectedClients(true);

  const uint64_t eventTs = timeKeeper_->nowUserEpoch() > 0 ? timeKeeper_->nowUserEpoch() : timeKeeper_->nowEpoch();

  JsonDocument event;
  event["type"] = "TIMER";
  event["event"] = "client.connected";
  event["msg"] = "Web client connected.";
  event["ts"] = eventTs;
  event["channel"] = -1;
  event["mac"] = mac;
  String eventLine;
  serializeJson(event, eventLine);
  enqueueEvent(eventLine, false);

  pushStateSnapshot(clientId);
  flushPendingToClient(clientId);

  JsonDocument request;
  request["type"] = "time_request";
  request["msg"] = "Please send current time via time_sync or /setTime.";
  request["ts"] = eventTs;
  String requestLine;
  serializeJson(request, requestLine);
  sendToClient(clientId, requestLine);
}

void WebPortal::onClientDisconnected(uint8_t clientId)
{
  String mac = "UNKNOWN";
  if (clientId < clients_.size())
  {
    if (contextMutex_ && xSemaphoreTake(contextMutex_, pdMS_TO_TICKS(50)) == pdTRUE)
    {
      clients_[clientId] = false;
      mac = clientMacs_[clientId].isEmpty() ? "UNKNOWN" : clientMacs_[clientId];
      clientMacs_[clientId] = "";
      xSemaphoreGive(contextMutex_);
    }
    else
    {
      mac = "UNKNOWN";
    }
  }
  syncConnectedClients(true);

  const uint64_t eventTs = timeKeeper_->nowUserEpoch() > 0 ? timeKeeper_->nowUserEpoch() : timeKeeper_->nowEpoch();

  JsonDocument event;
  event["type"] = "TIMER";
  event["event"] = "client.disconnected";
  event["msg"] = "Web client disconnected.";
  event["ts"] = eventTs;
  event["channel"] = -1;
  event["mac"] = mac;
  String eventLine;
  serializeJson(event, eventLine);
  enqueueEvent(eventLine, false);
}

bool WebPortal::enqueueInboundCommand(uint8_t clientId, const String &payload)
{
  if (!inboundQueue_)
  {
    return false;
  }
  if (payload.isEmpty() || payload.length() > MAX_WS_PAYLOAD_BYTES || containsBlockedInputTokens(payload))
  {
    if (socket_.clientIsConnected(clientId))
    {
      sendCommandAck(clientId, false, "Invalid command payload.");
    }
    return false;
  }

  if (payload.indexOf("\"type\":\"get_state\"") >= 0 && stateBroadcastPending_)
  {
    return true;
  }

  QueuedCommand queued{};
  queued.clientId = clientId;
  payload.substring(0, sizeof(queued.json) - 1).toCharArray(queued.json, sizeof(queued.json));

  if (xQueueSend(inboundQueue_, &queued, 0) == pdTRUE)
  {
    return true;
  }

  QueuedCommand dropped{};
  xQueueReceive(inboundQueue_, &dropped, 0);
  const bool queuedOk = xQueueSend(inboundQueue_, &queued, 0) == pdTRUE;
  if (!queuedOk && socket_.clientIsConnected(clientId))
  {
    sendCommandAck(clientId, false, "Controller busy, please retry.");
  }
  return queuedOk;
}

void WebPortal::handleClientMessage(uint8_t clientId, const String &payload)
{
  setCommandContext(clientId);
  CommandContextGuard guard(this);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err)
  {
    sendCommandAck(clientId, false, "Invalid JSON payload.");
    return;
  }

  const String type = doc["type"] | "";
  if (!isAllowedWsType(type))
  {
    sendCommandAck(clientId, false, "Unsupported command type.");
    return;
  }
  if (type == "time_sync")
  {
    if (!hasOnlyAllowedKeys(doc, {"type", "epoch", "year", "month", "day", "hours", "minutes", "seconds", "tzOffsetMinutes"}))
    {
      sendCommandAck(clientId, false, "Invalid time_sync payload.");
      return;
    }
    const bool ok = applyTimeSyncFromJson(doc, true);
    if (!ok)
    {
      sendCommandAck(clientId, false, "Invalid time_sync payload.");
      return;
    }
    sendCommandAck(clientId, true, "Time synchronized.");
    scheduleStateBroadcast();
    return;
  }

  if (type == "set_manual")
  {
    if (!hasOnlyAllowedKeys(doc, {"type", "channel", "mode"}))
    {
      sendCommandAck(clientId, false, "Invalid manual command.");
      return;
    }
    const String modeText = doc["mode"] | "";
    const int channelValue = doc["channel"] | -1;
    if (channelValue < 0 || !isAllowedRelayModeValue(modeText))
    {
      sendCommandAck(clientId, false, "Invalid manual command.");
      return;
    }
    const size_t channel = static_cast<size_t>(doc["channel"] | static_cast<int>(RELAY_COUNT));
    const RelayMode mode = relayModeFromText(modeText);
    if (shouldRateLimitRelayCommand(channel))
    {
      sendCommandAck(clientId, false, "Relay command rate limited, please retry.");
      return;
    }
    String errorText;
    const bool ok = engine_->setManualMode(channel, mode, &errorText);
    if (!ok && errorText == "Night Lock Active")
    {
      JsonDocument errDoc;
      errDoc["error"] = "Night Lock Active";
      String errPayload;
      serializeJson(errDoc, errPayload);
      sendToClient(clientId, errPayload);
      pushStateSnapshot(clientId);
      return;
    }
    sendCommandAck(clientId, ok, ok ? "Manual mode updated." : errorText);
    if (ok)
    {
      scheduleStateBroadcast();
    }
    return;
  }

  if (type == "set_timer")
  {
    if (!hasOnlyAllowedKeys(doc,
                            {"type", "channel", "durationMinutes", "durationSec", "target", "epoch", "year", "month",
                             "day", "hours", "minutes", "seconds", "tzOffsetMinutes"}))
    {
      sendCommandAck(clientId, false, "Invalid timer request.");
      return;
    }
    const String targetText = doc["target"] | "";
    const int channelValue = doc["channel"] | -1;
    if (channelValue < 0 || !isAllowedRelayStateValue(targetText))
    {
      sendCommandAck(clientId, false, "Invalid timer request.");
      return;
    }
    if (!applyTimeSyncFromJson(doc, false))
    {
      sendCommandAck(clientId, false, "Invalid time fields in timer request.");
      return;
    }
    const size_t channel = static_cast<size_t>(doc["channel"] | static_cast<int>(RELAY_COUNT));
    uint32_t durationMinutes = doc["durationMinutes"] | 0;
    if (durationMinutes == 0 && doc["durationSec"].is<uint32_t>())
    {
      const uint32_t durationSec = doc["durationSec"].as<uint32_t>();
      durationMinutes = max(1UL, (durationSec + 59UL) / 60UL);
    }
    const RelayState target = relayStateFromText(doc["target"] | "OFF");
    String errorText;
    const bool ok = engine_->setTimer(channel, durationMinutes, target, &errorText);
    if (!ok && errorText == "Night Lock Active")
    {
      JsonDocument errDoc;
      errDoc["error"] = "Night Lock Active";
      String errPayload;
      serializeJson(errDoc, errPayload);
      sendToClient(clientId, errPayload);
      pushStateSnapshot(clientId);
      return;
    }
    sendCommandAck(clientId, ok, ok ? "Timer saved." : errorText);
    if (ok)
    {
      scheduleStateBroadcast();
    }
    return;
  }

  if (type == "cancel_timer")
  {
    if (!hasOnlyAllowedKeys(doc, {"type", "channel"}) || (doc["channel"] | -1) < 0)
    {
      sendCommandAck(clientId, false, "Invalid timer request.");
      return;
    }
    const size_t channel = static_cast<size_t>(doc["channel"] | static_cast<int>(RELAY_COUNT));
    const bool ok = engine_->cancelTimer(channel);
    sendCommandAck(clientId, ok, ok ? "Timer canceled." : "No active timer on this relay.");
    if (ok)
    {
      scheduleStateBroadcast();
    }
    return;
  }

  if (type == "set_energy_tracking")
  {
    if (!hasOnlyAllowedKeys(doc, {"type", "enabled"}) || !doc["enabled"].is<bool>())
    {
      sendCommandAck(clientId, false, "Invalid energy tracking request.");
      return;
    }
    const bool enabled = doc["enabled"] | false;
    String errorText;
    const bool ok = engine_->setEnergyTrackingEnabled(enabled, &errorText);
    sendCommandAck(clientId, ok, ok ? String("Energy tracking ") + (enabled ? "enabled." : "disabled.") : errorText);
    if (ok)
    {
      scheduleStateBroadcast();
    }
    return;
  }

  if (type == "get_state")
  {
    if (!hasOnlyAllowedKeys(doc, {"type"}))
    {
      sendCommandAck(clientId, false, "Invalid state request.");
      return;
    }
    pushStateSnapshot(clientId);
    return;
  }
}

void WebPortal::sendToClient(uint8_t clientId, const String &json)
{
  if (clientId >= clients_.size())
  {
    return;
  }
  if (!socket_.clientIsConnected(clientId))
  {
    if (contextMutex_ && xSemaphoreTake(contextMutex_, pdMS_TO_TICKS(50)) == pdTRUE)
    {
      clients_[clientId] = false;
      clientMacs_[clientId] = "";
      xSemaphoreGive(contextMutex_);
    }
    syncConnectedClients(false);
    return;
  }
  String payload = json;
  if (!socket_.sendTXT(clientId, payload))
  {
    clients_[clientId] = false;
    clientMacs_[clientId] = "";
    socket_.disconnect(clientId);
    syncConnectedClients(false);
  }
}

void WebPortal::broadcast(const String &json)
{
  if (connectedClients_ == 0)
  {
    return;
  }
  String payload = json;
  if (!socket_.broadcastTXT(payload))
  {
    syncConnectedClients(false);
  }
}

void WebPortal::flushPendingToClient(uint8_t clientId)
{
  storage_->flushPending([&](const String &line)
                         { sendToClient(clientId, line); });
}

void WebPortal::sendCommandAck(uint8_t clientId, bool ok, const String &message)
{
  JsonDocument doc;
  doc["type"] = "command_ack";
  doc["ok"] = ok;
  doc["msg"] = message;
  const uint64_t ackTs = timeKeeper_->nowUserEpoch() > 0 ? timeKeeper_->nowUserEpoch() : timeKeeper_->nowEpoch();
  doc["ts"] = ackTs;
  String payload;
  serializeJson(doc, payload);
  sendToClient(clientId, payload);
}

void WebPortal::pushStateSnapshot(uint8_t clientId) { sendToClient(clientId, engine_->buildStateJson()); }

void WebPortal::processInboundCommands()
{
  if (!inboundQueue_)
  {
    return;
  }

  uint8_t processed = 0;
  QueuedCommand queued{};
  while (processed < 4 && xQueueReceive(inboundQueue_, &queued, 0) == pdTRUE)
  {
    const String payload(queued.json);
    handleClientMessage(queued.clientId, payload);
    ++processed;
  }
}

void WebPortal::processQueue()
{
  if (!outboundQueue_)
  {
    return;
  }

  bool snapshotNeeded = false;
  uint8_t processed = 0;
  QueuedEvent queued{};
  while (processed < 6 && xQueueReceive(outboundQueue_, &queued, 0) == pdTRUE)
  {
    const String raw(queued.json);
    const String fallbackMac(queued.mac);

    const String normalized = normalizeLogPayload(raw, fallbackMac);
    const bool persistToFlash = shouldPersistEventToFlash(normalized);

    if (persistToFlash)
    {
      storage_->appendEventJson(normalized);
    }
    if (connectedClients_ > 0)
    {
      broadcast(normalized);
      if (eventNeedsSnapshot(normalized))
      {
        snapshotNeeded = true;
      }
    }
    else if (queued.bufferIfOffline && persistToFlash)
    {
      storage_->appendPending(normalized);
    }
    ++processed;
  }

  if (snapshotNeeded)
  {
    scheduleStateBroadcast();
  }
}

void WebPortal::scheduleStateBroadcast() { stateBroadcastPending_ = true; }

void WebPortal::processPendingStateBroadcast()
{
  if (!stateBroadcastPending_ || connectedClients_ == 0)
  {
    return;
  }
  stateBroadcastPending_ = false;
  broadcast(engine_->buildStateJson());
}

bool WebPortal::applyTimeSyncFromJson(const JsonDocument &doc, bool requireClockFields)
{
  if (!doc["tzOffsetMinutes"].isNull())
  {
    if (!doc["tzOffsetMinutes"].is<int>() ||
        !timeKeeper_->setTimezoneOffsetMinutes(doc["tzOffsetMinutes"].as<int32_t>()))
    {
      return false;
    }
  }

  if (doc["epoch"].is<uint64_t>())
  {
    return timeKeeper_->syncFromClient(doc["epoch"].as<uint64_t>());
  }
  if (doc["year"].is<int>() && doc["month"].is<int>() && doc["day"].is<int>())
  {
    return timeKeeper_->syncFromDateTime(doc["year"] | 0,
                                         doc["month"] | 0,
                                         doc["day"] | 0,
                                         doc["hours"] | 0,
                                         doc["minutes"] | 0,
                                         doc["seconds"] | 0);
  }
  if (doc["hours"].is<int>() || doc["minutes"].is<int>() || doc["seconds"].is<int>())
  {
    return timeKeeper_->syncFromHms(doc["hours"] | -1, doc["minutes"] | -1, doc["seconds"] | -1);
  }

  return !requireClockFields;
}

String WebPortal::normalizeLogPayload(const String &rawJson, const String &fallbackMac) const
{
  JsonDocument in;
  if (deserializeJson(in, rawJson))
  {
    JsonDocument out;
    out["type"] = "ERROR";
    out["relay"] = -1;
    out["message"] = "Malformed log payload received.";
    out["msg"] = out["message"];
    const uint64_t fallbackTs = timeKeeper_->nowUserEpoch() > 0 ? timeKeeper_->nowUserEpoch() : timeKeeper_->nowEpoch();
    out["ts"] = fallbackTs;
    out["time"] = formatEpochClock(out["ts"].as<uint64_t>());
    out["mac"] = fallbackMac.isEmpty() ? "SYSTEM" : fallbackMac;
    String payload;
    serializeJson(out, payload);
    return payload;
  }

  uint64_t ts = in["ts"].is<uint64_t>() ? in["ts"].as<uint64_t>() : 0;
  if (ts < 1700000000ULL)
  {
    ts = timeKeeper_->nowUserEpoch() > 0 ? timeKeeper_->nowUserEpoch() : timeKeeper_->nowEpoch();
  }
  const String type = in["type"] | "TIMER";
  const String message = in["message"] | (in["msg"] | "");
  const int relay = in["relay"].is<int>() ? in["relay"].as<int>() : (in["channel"].is<int>() ? in["channel"].as<int>() : -1);

  String mac = in["mac"] | "";
  if (mac.isEmpty())
  {
    mac = fallbackMac;
  }
  if (mac.isEmpty())
  {
    mac = "SYSTEM";
  }

  JsonDocument out;
  out["type"] = type;
  out["relay"] = relay;
  out["message"] = message;
  out["msg"] = message;
  out["ts"] = ts;
  out["time"] = formatEpochClock(ts);
  out["mac"] = mac;

  const String eventName = in["event"] | "";
  if (!eventName.isEmpty())
  {
    out["event"] = eventName;
  }
  if (relay >= 0)
  {
    out["channel"] = relay;
  }
  if (in["lastWh"].is<float>() || in["lastWh"].is<double>())
  {
    out["lastWh"] = in["lastWh"].as<float>();
  }
  if (in["totalWh"].is<float>() || in["totalWh"].is<double>())
  {
    out["totalWh"] = in["totalWh"].as<float>();
  }

  String payload;
  serializeJson(out, payload);
  return payload;
}

String WebPortal::formatEpochClock(uint64_t epoch) const
{
  if (epoch == 0)
  {
    return "--:--:--";
  }
  time_t raw = static_cast<time_t>(epoch);
  struct tm info;
#if defined(ESP32)
  if (!localtime_r(&raw, &info))
  {
    return "--:--:--";
  }
#else
  info = *localtime(&raw);
#endif
  char buff[12];
  snprintf(buff, sizeof(buff), "%02d:%02d:%02d", info.tm_hour, info.tm_min, info.tm_sec);
  return String(buff);
}

void WebPortal::setCommandContext(uint8_t clientId)
{
  String mac = resolveClientMac(clientId);
  if (mac.isEmpty())
  {
    mac = "UNKNOWN";
  }
  if (clientId < clientMacs_.size())
  {
    clientMacs_[clientId] = mac;
  }
  if (!contextMutex_)
  {
    return;
  }
  if (xSemaphoreTake(contextMutex_, pdMS_TO_TICKS(50)) != pdTRUE)
  {
    return;
  }
  commandContextActive_ = true;
  commandContextTask_ = xTaskGetCurrentTaskHandle();
  commandContextMac_ = mac;
  commandContextExpiryMs_ = millis() + 1600;
  xSemaphoreGive(contextMutex_);
}

void WebPortal::clearCommandContext()
{
  if (!contextMutex_)
  {
    return;
  }
  if (xSemaphoreTake(contextMutex_, pdMS_TO_TICKS(50)) != pdTRUE)
  {
    return;
  }
  commandContextTask_ = nullptr;
  xSemaphoreGive(contextMutex_);
}

String WebPortal::activeCommandMac()
{
  if (!contextMutex_)
  {
    return "SYSTEM";
  }
  String mac = "SYSTEM";
  if (xSemaphoreTake(contextMutex_, pdMS_TO_TICKS(10)) != pdTRUE)
  {
    return mac;
  }
  const uint32_t nowMs = millis();
  const bool expired = commandContextActive_ && static_cast<int32_t>(nowMs - commandContextExpiryMs_) > 0;
  if (expired)
  {
    commandContextActive_ = false;
    commandContextMac_ = "SYSTEM";
    commandContextExpiryMs_ = 0;
  }
  if (commandContextActive_ && !commandContextMac_.isEmpty())
  {
    mac = commandContextMac_;
  }
  else
  {
    for (size_t i = 0; i < clients_.size(); ++i)
    {
      if (clients_[i] && !clientMacs_[i].isEmpty())
      {
        mac = clientMacs_[i];
        break;
      }
    }
  }
  xSemaphoreGive(contextMutex_);
  return mac;
}

String WebPortal::resolveClientMac(uint8_t clientId)
{
  if (clientId < clientMacs_.size() && !clientMacs_[clientId].isEmpty())
  {
    return clientMacs_[clientId];
  }

  const IPAddress remoteIp = socket_.remoteIP(clientId);
  wifi_sta_list_t wifiStaList;
  memset(&wifiStaList, 0, sizeof(wifiStaList));
  if (esp_wifi_ap_get_sta_list(&wifiStaList) != ESP_OK)
  {
    return "UNKNOWN";
  }

  esp_netif_sta_list_t netifStaList;
  memset(&netifStaList, 0, sizeof(netifStaList));
  if (esp_netif_get_sta_list(&wifiStaList, &netifStaList) == ESP_OK)
  {
    for (int i = 0; i < netifStaList.num; ++i)
    {
      const uint32_t addr = netifStaList.sta[i].ip.addr;
      const IPAddress normal = ipFromAddr(addr, false);
      const IPAddress reversed = ipFromAddr(addr, true);
      if (remoteIp == normal || remoteIp == reversed)
      {
        return formatMac(netifStaList.sta[i].mac);
      }
    }
  }

  if (wifiStaList.num == 1)
  {
    return formatMac(wifiStaList.sta[0].mac);
  }
  return "UNKNOWN";
}
String WebPortal::resolveClientMacFromIP(IPAddress ip)
{
  wifi_sta_list_t wifiStaList;
  memset(&wifiStaList, 0, sizeof(wifiStaList));
  if (esp_wifi_ap_get_sta_list(&wifiStaList) != ESP_OK)
    return "UNKNOWN";

  esp_netif_sta_list_t netifStaList;
  memset(&netifStaList, 0, sizeof(netifStaList));
  if (esp_netif_get_sta_list(&wifiStaList, &netifStaList) != ESP_OK)
    return "UNKNOWN";

  for (int i = 0; i < netifStaList.num; ++i)
  {
    const uint32_t addr = netifStaList.sta[i].ip.addr;
    if (ip == ipFromAddr(addr, false) || ip == ipFromAddr(addr, true))
    {
      return formatMac(wifiStaList.sta[i].mac);
    }
  }
  return "UNKNOWN";
}

String WebPortal::macFromHttpClient()
{
  return resolveClientMacFromIP(server_.client().remoteIP());
}
String WebPortal::formatMac(const uint8_t mac[6])
{
  if (!mac)
  {
    return "UNKNOWN";
  }
  char buff[20];
  snprintf(buff, sizeof(buff), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buff);
}

IPAddress WebPortal::ipFromAddr(uint32_t addr, bool reverseOrder)
{
  if (reverseOrder)
  {
    return IPAddress((addr) & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF, (addr >> 24) & 0xFF);
  }
  return IPAddress((addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, (addr) & 0xFF);
}

String WebPortal::handleSetTime(const String &body)
{
  JsonDocument response;
  response["type"] = "set_time_ack";
  response["ok"] = false;

  JsonDocument doc;
  if (deserializeJson(doc, body))
  {
    response["status"] = "ERROR";
    response["msg"] = "Invalid JSON body.";
    String payload;
    serializeJson(response, payload);
    return payload;
  }

  const bool ok = applyTimeSyncFromJson(doc, true);

  response["ok"] = ok;
  response["status"] = ok ? "OK" : "ERROR";
  response["epoch"] = timeKeeper_->nowEpoch();
  response["dayPhase"] = dayPhaseToText(timeKeeper_->currentDayPhase());
  response["msg"] = ok ? "Clock synchronized." : "Could not parse/validate supplied time.";

  if (ok)
  {
    JsonDocument event;
    event["type"] = "TIMER";
    event["event"] = "time.sync";
    event["msg"] = "Device time synced from web client.";
    event["ts"] = timeKeeper_->nowUserEpoch() > 0 ? timeKeeper_->nowUserEpoch() : timeKeeper_->nowEpoch();
    event["channel"] = -1;
    event["mac"] = "SYSTEM";
    String line;
    serializeJson(event, line);
    enqueueEvent(line, false);
  }

  String payload;
  serializeJson(response, payload);
  return payload;
}

uint16_t WebPortal::recalcConnectedClients()
{
  wifi_sta_list_t wifiStaList;
  memset(&wifiStaList, 0, sizeof(wifiStaList));
  const bool haveStationList = esp_wifi_ap_get_sta_list(&wifiStaList) == ESP_OK;

  std::array<String, WS_MAX_CLIENTS> countedDevices{};
  uint16_t count = 0;

  if (contextMutex_ && xSemaphoreTake(contextMutex_, pdMS_TO_TICKS(25)) == pdTRUE)
  {
    for (size_t i = 0; i < clients_.size(); ++i)
    {
      if (!clients_[i])
      {
        continue;
      }

      if (clientMacs_[i].isEmpty() || clientMacs_[i] == "UNKNOWN")
      {
        clientMacs_[i] = resolveClientMac(static_cast<uint8_t>(i));
      }

      bool stationActive = !haveStationList;
      if (haveStationList)
      {
        stationActive = false;
        for (int s = 0; s < wifiStaList.num; ++s)
        {
          if (clientMacs_[i].equalsIgnoreCase(formatMac(wifiStaList.sta[s].mac)))
          {
            stationActive = true;
            break;
          }
        }
      }

      if (!stationActive)
      {
        clients_[i] = false;
        clientMacs_[i] = "";
        continue;
      }

      String deviceKey = clientMacs_[i];
      if (deviceKey.isEmpty() || deviceKey == "UNKNOWN")
      {
        deviceKey = String("ws:") + String(i);
      }

      bool seen = false;
      for (size_t existing = 0; existing < count; ++existing)
      {
        if (countedDevices[existing].equalsIgnoreCase(deviceKey))
        {
          seen = true;
          break;
        }
      }
      if (!seen && count < WS_MAX_CLIENTS)
      {
        countedDevices[count++] = deviceKey;
      }
    }
    xSemaphoreGive(contextMutex_);
  }
  else
  {
    return connectedClients_;
  }
  return count;
}

void WebPortal::syncConnectedClients(bool broadcastSnapshot)
{
  const uint16_t liveCount = recalcConnectedClients();
  if (liveCount == connectedClients_)
  {
    return;
  }

  connectedClients_ = liveCount;
  updateClientCountInEngine();

  if (broadcastSnapshot && connectedClients_ > 0)
  {
    scheduleStateBroadcast();
  }
}

void WebPortal::updateClientCountInEngine() { engine_->updateConnectedClients(connectedClients_); }

void WebPortal::onWsEventStatic(uint8_t clientId, WStype_t type, uint8_t *payload, size_t length)
{
  if (instance_)
  {
    instance_->handleWsEvent(clientId, type, payload, length);
  }
}

// ============================================================
// ACCESS CONTROL HELPER METHODS
// ============================================================

bool WebPortal::validateMacAccess(const String &macAddress)
{
  if (!ENABLE_ACCESS_CONTROL)
    return true;

  // FIX RC-6: never fail open. Previously, when WiFi was in STA-only mode and
  // the MAC could not be resolved, the gate returned true, which silently
  // disabled access control. If we cannot identify the client we must deny.
  if (macAddress.isEmpty() || macAddress == "UNKNOWN")
    return false;

  for (uint8_t i = 0; i < accessControl_.userCount; i++)
  {
    if (strcmp(accessControl_.users[i].macAddress, macAddress.c_str()) == 0)
    {
      return true;
    }
  }
  return false;
}

bool WebPortal::authenticateUser(const String &macAddress, const String &password)
{
  if (!ENABLE_ACCESS_CONTROL)
    return true;

  for (uint8_t i = 0; i < accessControl_.userCount; i++)
  {
    if (strcmp(accessControl_.users[i].macAddress, macAddress.c_str()) == 0)
    {
      // Hash the input password and compare
      uint8_t hash[32];
      mbedtls_sha256_ret(reinterpret_cast<const unsigned char *>(password.c_str()),
                         password.length(), hash, 0);
      char hexString[65];
      for (size_t j = 0; j < 32; j++)
      {
        snprintf(hexString + (j * 2), 3, "%02x", hash[j]);
      }
      hexString[64] = '\0';

      if (strcmp(accessControl_.users[i].passwordHash, hexString) == 0)
      {
        return true;
      }
    }
  }
  return false;
}

bool WebPortal::checkPermission(const UserAccount &user, const String &requiredPermission)
{
  if (requiredPermission == "admin")
    return user.isAdmin;
  if (requiredPermission == "manageUsers")
    return user.isAdmin || user.canManageUsers;
  return true;
}

void WebPortal::updateLastAccess(const char *macAddress)
{
  for (uint8_t i = 0; i < accessControl_.userCount; i++)
  {
    if (strcmp(accessControl_.users[i].macAddress, macAddress) == 0)
    {
      accessControl_.users[i].lastAccess = time(nullptr);
      storage_->saveUserAccounts(&accessControl_);
      return;
    }
  }
}

String WebPortal::getConnectedClientMac(uint8_t clientId)
{
  return resolveClientMac(clientId);
}

bool WebPortal::isAuthorizedRoute(const String &uri)
{
  // Public routes that don't require authentication
  if (uri == "/" || uri == "/index.html" ||
      uri.startsWith("/generate_204") || uri.startsWith("/fwlink") ||
      uri == "/unauthorized.html" || uri == "/restricted.html" ||
      uri.startsWith("/api/auth/"))
  {
    return true;
  }
  return false;
}
