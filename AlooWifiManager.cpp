#include "AlooWifiManager.h"
#include <DNSServer.h>

// Static member initialization
constexpr char WiFiManager::PREF_NAMESPACE[];
constexpr char WiFiManager::PREF_SSID_KEY[];
constexpr char WiFiManager::PREF_PASS_KEY[];

////////////////////////////////////////////////////////////
// Constructor & Destructor
////////////////////////////////////////////////////////////
WiFiManager::WiFiManager(const String& apSsid, const String& apPassword)
  : _apSsid(apSsid),
    _apPassword(apPassword),
    _status(WiFiStatus::INITIALIZING),
    _pendingSsid(""),
    _pendingPassword(""),
    _newCredentialsAvailable(false),
    _server(nullptr),
    _runServerOnSeparateCore(false),
    _managerTaskHandle(nullptr),
    _serverTaskHandle(nullptr),
    _serverCore(1),
    _managerCore(1)
{
  _statusMutex = xSemaphoreCreateMutex();
  _pendingMutex = xSemaphoreCreateMutex();
}

WiFiManager::~WiFiManager() {
  if (_managerTaskHandle) vTaskDelete(_managerTaskHandle);
  if (_serverTaskHandle) vTaskDelete(_serverTaskHandle);
  if (_statusMutex) vSemaphoreDelete(_statusMutex);
  if (_pendingMutex) vSemaphoreDelete(_pendingMutex);
  stopAPMode();
}

////////////////////////////////////////////////////////////
// Credential Storage Helpers
////////////////////////////////////////////////////////////
bool WiFiManager::loadLastCredentials(String &ssid, String &password) {
  if (!_preferences.begin(PREF_NAMESPACE, true)) { // Read-only mode
    Serial.println("WiFiManager: Failed to initialize preferences (read-only).");
    return false;
  }
  ssid = _preferences.getString(PREF_SSID_KEY, "");
  password = _preferences.getString(PREF_PASS_KEY, "");
  _preferences.end();
  return (!ssid.isEmpty() && !password.isEmpty());
}

bool WiFiManager::saveLastCredentials(const String &ssid, const String &password) {
  if (!_preferences.begin(PREF_NAMESPACE, false)) { // Read-write mode
    Serial.println("WiFiManager: Failed to initialize preferences (read-write).");
    return false;
  }
  bool success = true;
  success &= _preferences.putString(PREF_SSID_KEY, ssid);
  success &= _preferences.putString(PREF_PASS_KEY, password);
  _preferences.end();
  
  if (success) {
    Serial.println("WiFiManager: Credentials saved to preferences.");
  } else {
    Serial.println("WiFiManager: Failed to save credentials.");
  }
  return success;
}

////////////////////////////////////////////////////////////
// WiFi Connection Helper
////////////////////////////////////////////////////////////
bool WiFiManager::tryConnect(const String &ssid, const String &password) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100); // Allow time for any previous connection to tear down

  Serial.printf("WiFiManager: Attempting to connect to '%s'\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), password.c_str());
  const unsigned long timeout = 15000; // 15-second timeout
  unsigned long startTime = millis();

  while (millis() - startTime < timeout) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFiManager: Successfully connected to '%s'\n", ssid.c_str());
      saveLastCredentials(ssid, password);
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  
  Serial.printf("WiFiManager: Connection attempt to '%s' failed\n", ssid.c_str());
  return false;
}

////////////////////////////////////////////////////////////
// AP Mode & Captive Portal
////////////////////////////////////////////////////////////
void WiFiManager::setupCaptivePortal() {
  // Setup DNS server for captive portal redirection
  _dnsServer.start(53, "*", WiFi.softAPIP());

  // Setup captive portal detection endpoints
  _server->on("/generate_204", [this]() { handleRedirect(); });      // Android
  _server->on("/hotspot-detect.html", [this]() { handleRedirect(); }); // Apple
  _server->on("/connecttest.txt", [this]() { handleRedirect(); });     // Windows
  _server->on("/ncsi.txt", [this]() { handleRedirect(); });            // Microsoft

  // Redirect all other requests
  _server->onNotFound([this]() {
    if (!isIp(_server->hostHeader())) {
      handleRedirect();
    } else {
      _server->send(404, "text/plain", "404: Not Found");
    }
  });
}
void WiFiManager::handleRedirect() {
  String redirectUrl = "http://" + _server->client().localIP().toString() + "/";
  _server->sendHeader("Location", redirectUrl);
  _server->send(302, "text/plain", "Redirecting to setup portal");
}

bool WiFiManager::isIp(const String& str) {
  for (size_t i = 0; i < str.length(); i++) {
    if (isDigit(str[i]) || str[i] == '.') continue;
    return false;
  }
  return true;
}
void WiFiManager::startAPMode() {
  // If already in AP mode with an active server, return early.
  if (WiFi.getMode() == WIFI_AP && _server != nullptr) {
    return;
  }

  Serial.println("WiFiManager: Starting AP mode for WiFi setup...");

  // Set WiFi mode to AP and start the soft AP. If a valid password is provided
  // (at least 8 characters), use it; otherwise, start an open network.
  WiFi.mode(WIFI_AP);
  if (_apPassword.length() >= 8) {
    WiFi.softAP(_apSsid.c_str(), _apPassword.c_str());
  } else {
    WiFi.softAP(_apSsid.c_str());
  }

  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("WiFiManager: AP IP: %s\n", apIP.toString().c_str());

  // Create and configure the web server.
  if (_server) {
    delete _server;
    _server = nullptr;
  }
  _server = new WebServer(80);

  // Setup HTTP handlers for captive portal and configuration endpoints.
  _server->on("/", [this]() { handleRoot(); });
  _server->on("/connect", [this]() { handleConnectPage(); });
  _server->on("/submit", HTTP_POST, [this]() { handleSubmitCredentials(); });

  // Setup captive portal endpoints.
  setupCaptivePortal();

  _server->begin();

  // Optionally, if running the server on a separate core, create the server task.
  if (_runServerOnSeparateCore && !_serverTaskHandle) {
    BaseType_t result = xTaskCreatePinnedToCore(
      serverTask,
      "WiFiServerTask",
      4096,
      this,
      1,
      &_serverTaskHandle,
      _serverCore
    );
    if (result != pdPASS) {
      Serial.println("WiFiManager: Failed to create server task");
      _serverTaskHandle = nullptr;
    }
  }
}
void WiFiManager::stopAPMode() {
  Serial.println("WiFiManager: Stopping AP mode");
  _dnsServer.stop();
  if (_server) {
    _server->stop();
    delete _server;
    _server = nullptr;
  }
  if (WiFi.getMode() == WIFI_AP) {
    WiFi.softAPdisconnect(true);
  }
  if (_serverTaskHandle) {
    vTaskDelete(_serverTaskHandle);
    _serverTaskHandle = nullptr;
  }
}

////////////////////////////////////////////////////////////
// Captive Portal HTTP Handlers
////////////////////////////////////////////////////////////
void WiFiManager::handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>ESP32 WiFi Setup</title></head><body>";
  html += "<h1>ESP32 WiFi Setup</h1>";
  html += "<button onclick=\"window.location.href='/connect'\">Setup WiFi</button>";
  html += "</body></html>";
  _server->send(200, "text/html", html);
}

void WiFiManager::handleConnectPage() {
  int n = WiFi.scanNetworks();
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Select WiFi Network</title>";
  html += "<script>"
          "function fillSSID(ssid) { document.getElementById('ssid').value = ssid; document.getElementById('password').focus(); }"
          "function togglePassword() { var x = document.getElementById('password'); x.type = (x.type === 'password') ? 'text' : 'password'; }"
          "</script>";
  html += "</head><body>";
  html += "<h1>Select WiFi Network</h1>";
  if (n == 0) {
    html += "<p>No networks found. Please refresh.</p>";
  } else {
    html += "<ul>";
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      int32_t rssi = WiFi.RSSI(i);
      html += "<li onclick=\"fillSSID('" + ssid + "')\" style='cursor:pointer;'>";
      html += ssid + " (" + String(rssi) + " dBm)";
      html += "</li>";
    }
    html += "</ul>";
  }
  html += "<form action='/submit' method='POST'>";
  html += "SSID: <input type='text' id='ssid' name='ssid'><br>";
  html += "Password: <input type='password' id='password' name='password'><br>";
  html += "<input type='checkbox' onclick='togglePassword()'> Show Password<br>";
  html += "<input type='submit' value='Connect'>";
  html += "</form>";
  html += "</body></html>";
  _server->send(200, "text/html", html);
  WiFi.scanDelete(); // Free scan results
}

void WiFiManager::handleSubmitCredentials() {
  // Validate that an SSID was provided
  if (!_server->hasArg("ssid") || _server->arg("ssid") == "") {
    _server->send(400, "text/plain", "SSID is required.");
    return;
  }
  String ssid = _server->arg("ssid");
  String password = _server->arg("password");

  // Save the submitted credentials in a thread-safe way
  if (xSemaphoreTake(_pendingMutex, portMAX_DELAY) == pdTRUE) {
    _pendingSsid = ssid;
    _pendingPassword = password;
    _newCredentialsAvailable = true;
    xSemaphoreGive(_pendingMutex);
  }

  // Immediately respond to the client
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Connecting</title></head><body>";
  html += "<h1>Attempting to connect...</h1>";
  html += "<p>Please wait while the connection is attempted.</p>";
  html += "</body></html>";
  _server->send(200, "text/html", html);
}

////////////////////////////////////////////////////////////
// Manager Task (Asynchronous WiFi Logic)
////////////////////////////////////////////////////////////
void WiFiManager::managerTask(void* param) {
  WiFiManager* manager = static_cast<WiFiManager*>(param);
  String storedSsid, storedPassword;

  // Attempt connection using stored credentials (if available).
  if (manager->loadLastCredentials(storedSsid, storedPassword)) {
    // Update state: trying to connect.
    if (xSemaphoreTake(manager->_statusMutex, portMAX_DELAY) == pdTRUE) {
      manager->_status = WiFiStatus::TRYING_TO_CONNECT;
      xSemaphoreGive(manager->_statusMutex);
    }
    if (manager->tryConnect(storedSsid, storedPassword)) {
      // Connection successful.
      if (xSemaphoreTake(manager->_statusMutex, portMAX_DELAY) == pdTRUE) {
        manager->_status = WiFiStatus::CONNECTED;
        xSemaphoreGive(manager->_statusMutex);
      }
      Serial.println("WiFiManager: Connected using stored credentials.");
      manager->stopAPMode();
      vTaskDelete(NULL);
      return;
    } else {
      Serial.println("WiFiManager: Stored credentials failed.");
    }
  } else {
    Serial.println("WiFiManager: No stored credentials found.");
  }

  // Since stored credentials did not yield a connection, ensure AP mode is active.
  manager->ensureAPModeActive();

  // Main loop: Poll for new credentials submitted via the captive portal.
  while (true) {
    // If connected by any means, exit the loop.
    if (WiFi.status() == WL_CONNECTED) {
      if (xSemaphoreTake(manager->_statusMutex, portMAX_DELAY) == pdTRUE) {
        manager->_status = WiFiStatus::CONNECTED;
        xSemaphoreGive(manager->_statusMutex);
      }
      Serial.println("WiFiManager: WiFi connection established.");
      manager->stopAPMode();
      break;
    }

    // Check (in a thread-safe way) for new credentials submitted from the web server.
    bool newCreds = false;
    String newSsid, newPassword;
    if (xSemaphoreTake(manager->_pendingMutex, portMAX_DELAY) == pdTRUE) {
      if (manager->_newCredentialsAvailable) {
        newCreds = true;
        newSsid = manager->_pendingSsid;
        newPassword = manager->_pendingPassword;
        // Clear the flag after consuming the new credentials.
        manager->_newCredentialsAvailable = false;
      }
      xSemaphoreGive(manager->_pendingMutex);
    }

    if (newCreds) {
      // Before attempting a connection, ensure AP mode is active so that the captive portal is accessible.
      manager->ensureAPModeActive();

      // Update state: trying to connect with new credentials.
      if (xSemaphoreTake(manager->_statusMutex, portMAX_DELAY) == pdTRUE) {
        manager->_status = WiFiStatus::TRYING_TO_CONNECT;
        xSemaphoreGive(manager->_statusMutex);
      }
      Serial.printf("WiFiManager: Attempting connection with new credentials: '%s'\n", newSsid.c_str());
      if (manager->tryConnect(newSsid, newPassword)) {
        // Successful connection.
        if (xSemaphoreTake(manager->_statusMutex, portMAX_DELAY) == pdTRUE) {
          manager->_status = WiFiStatus::CONNECTED;
          xSemaphoreGive(manager->_statusMutex);
        }
        Serial.println("WiFiManager: Connected using new credentials.");
        manager->stopAPMode();
        break;
      } else {
        // Connection failed; log and re-enable AP mode for further attempts.
        Serial.println("WiFiManager: New credentials failed to connect; reverting to AP mode.");
        manager->ensureAPModeActive();
      }
    }

    // Short delay before next poll iteration.
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // Delete the manager task when finished.
  vTaskDelete(NULL);
}
////////////////////////////////////////////////////////////
// Server Task (Runs the Web Server on a Separate Core)
////////////////////////////////////////////////////////////
void WiFiManager::serverTask(void* param) {
  WiFiManager* manager = static_cast<WiFiManager*>(param);
  for (;;) {
    if (manager->_server) {
      manager->_server->handleClient();
      manager->_dnsServer.processNextRequest();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

////////////////////////////////////////////////////////////
// Public API Methods
////////////////////////////////////////////////////////////
void WiFiManager::begin(bool runServerOnSeparateCore, int serverCore, int managerCore) {
  _runServerOnSeparateCore = runServerOnSeparateCore;
  _serverCore = serverCore;
  _managerCore = managerCore;

  Serial.println("WiFiManager: Starting asynchronous initialization...");

  BaseType_t result = xTaskCreatePinnedToCore(
    managerTask,
    "WiFiManagerTask",
    8192,   // Adequate stack size
    this,
    1,      // Task priority
    &_managerTaskHandle,
    _managerCore
  );
  if (result != pdPASS) {
    Serial.println("WiFiManager: Failed to create manager task.");
    _managerTaskHandle = nullptr;
  }
}

WiFiStatus WiFiManager::getStatus() {
  WiFiStatus currentStatus;
  if (xSemaphoreTake(_statusMutex, portMAX_DELAY) == pdTRUE) {
    currentStatus = _status;
    xSemaphoreGive(_statusMutex);
  }
  return currentStatus;
}

void WiFiManager::processWebServer() {
  if (!_runServerOnSeparateCore && _server) {
    _server->handleClient();
  }
}

bool WiFiManager::resetCredentials() {
  if (!_preferences.begin(PREF_NAMESPACE, false)) {
    Serial.println("WiFiManager: Failed to initialize preferences for reset.");
    return false;
  }
  bool success = _preferences.clear();
  _preferences.end();
  if (success) {
    Serial.println("WiFiManager: Credentials reset successfully.");
  } else {
    Serial.println("WiFiManager: Failed to reset credentials.");
  }
  return success;
}

void WiFiManager::ensureAPModeActive() {
  // Check if we are already in AP mode and if the web server is running.
  if (WiFi.getMode() == WIFI_AP && _server != nullptr) {
    // Already active; nothing to do.
    return;
  }
  // Otherwise, (re)start AP mode.
  startAPMode();
  // Update the state accordingly.
  if (xSemaphoreTake(_statusMutex, portMAX_DELAY) == pdTRUE) {
    _status = WiFiStatus::AP_MODE_ACTIVE;
    xSemaphoreGive(_statusMutex);
  }
}