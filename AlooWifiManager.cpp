#include "AlooWifiManager.h"
#include <DNSServer.h>

// Static member initialization for preference keys
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
    _monitorTaskHandle(nullptr),
    _serverCore(1),
    _managerCore(1),
    _connectTimeout(15000)  // Default connection timeout: 15 seconds
{
  _statusMutex   = xSemaphoreCreateMutex();
  _pendingMutex  = xSemaphoreCreateMutex();
  _connectionMutex = xSemaphoreCreateMutex();
}

WiFiManager::~WiFiManager() {
  // Delete any running tasks
  if (_managerTaskHandle) vTaskDelete(_managerTaskHandle);
  if (_serverTaskHandle) vTaskDelete(_serverTaskHandle);
  if (_monitorTaskHandle) vTaskDelete(_monitorTaskHandle);
  // Delete semaphores
  if (_statusMutex) vSemaphoreDelete(_statusMutex);
  if (_pendingMutex) vSemaphoreDelete(_pendingMutex);
  if (_connectionMutex) vSemaphoreDelete(_connectionMutex);
  stopAPMode();
}

////////////////////////////////////////////////////////////
// Public API Methods
////////////////////////////////////////////////////////////
void WiFiManager::begin(bool runServerOnSeparateCore, int serverCore, int managerCore) {
  _runServerOnSeparateCore = runServerOnSeparateCore;
  _serverCore = serverCore;
  _managerCore = managerCore;

  Serial.println("WiFiManager: Starting asynchronous initialization...");

  // Create the manager task for initial connection and credential handling.
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

  // Create the monitor task for automatic reconnection.
  result = xTaskCreatePinnedToCore(
    monitorTask,
    "WiFiMonitorTask",
    4096,   // Stack size for monitoring
    this,
    1,      // Task priority
    &_monitorTaskHandle,
    _managerCore
  );
  if (result != pdPASS) {
    Serial.println("WiFiManager: Failed to create monitor task.");
    _monitorTaskHandle = nullptr;
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
  // Process clients if the web server is running on the main core.
  if (!_runServerOnSeparateCore && _server) {
    _server->handleClient();
    _dnsServer.processNextRequest();
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

void WiFiManager::setConnectTimeout(unsigned long timeout) {
  _connectTimeout = timeout;
}

////////////////////////////////////////////////////////////
// Credential Storage Helpers
////////////////////////////////////////////////////////////
bool WiFiManager::loadLastCredentials(String &ssid, String &password) {
  if (!_preferences.begin(PREF_NAMESPACE, true)) { // Open preferences in read-only mode
    Serial.println("WiFiManager: Failed to initialize preferences (read-only).");
    return false;
  }
  ssid = _preferences.getString(PREF_SSID_KEY, "");
  password = _preferences.getString(PREF_PASS_KEY, "");
  _preferences.end();
  return (!ssid.isEmpty() && !password.isEmpty());
}

bool WiFiManager::saveLastCredentials(const String &ssid, const String &password) {
  if (!_preferences.begin(PREF_NAMESPACE, false)) { // Open preferences in read-write mode
    Serial.println("WiFiManager: Failed to initialize preferences (read-write).");
    return false;
  }
  bool ssidSuccess = _preferences.putString(PREF_SSID_KEY, ssid);
  bool passSuccess = _preferences.putString(PREF_PASS_KEY, password);
  _preferences.end();

  if (ssidSuccess && passSuccess) {
    Serial.println("WiFiManager: Credentials saved to preferences.");
  } else {
    Serial.println("WiFiManager: Failed to save credentials properly.");
  }
  return ssidSuccess && passSuccess;
}

////////////////////////////////////////////////////////////
// WiFi Connection Helper (with connection mutex and configurable timeout)
////////////////////////////////////////////////////////////
bool WiFiManager::tryConnect(const String &ssid, const String &password) {
  // Prevent simultaneous connection attempts.
  if (xSemaphoreTake(_connectionMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("WiFiManager: Connection attempt already in progress.");
    return false;
  }

  // Set WiFi mode to STA and disconnect any previous connections.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100); // Give time for any previous connection to tear down.

  Serial.printf("WiFiManager: Attempting to connect to '%s'\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long startTime = millis();
  bool connected = false;

  // Wait until connected or until the configured timeout expires.
  while (millis() - startTime < _connectTimeout) {
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (connected) {
    Serial.printf("WiFiManager: Successfully connected to '%s'\n", ssid.c_str());
    saveLastCredentials(ssid, password);
  } else {
    Serial.printf("WiFiManager: Connection attempt to '%s' failed\n", ssid.c_str());
  }

  xSemaphoreGive(_connectionMutex);
  return connected;
}

////////////////////////////////////////////////////////////
// AP Mode & Captive Portal Functions
////////////////////////////////////////////////////////////
void WiFiManager::setupCaptivePortal() {
  // Start DNS server to redirect all requests to the captive portal.
  _dnsServer.start(53, "*", WiFi.softAPIP());

  // Register endpoints used by various devices to detect captive portals.
  _server->on("/generate_204", [this]() { handleRedirect(); });
  _server->on("/hotspot-detect.html", [this]() { handleRedirect(); });
  _server->on("/connecttest.txt", [this]() { handleRedirect(); });
  _server->on("/ncsi.txt", [this]() { handleRedirect(); });

  // Catch-all handler: redirect any undefined request.
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
  // If already in AP mode with an active web server, do nothing.
  if (WiFi.getMode() == WIFI_AP && _server != nullptr) {
    return;
  }

  Serial.println("WiFiManager: Starting AP mode for WiFi setup...");

  // Switch WiFi to AP mode and start the soft AP. Use password if at least 8 characters.
  WiFi.mode(WIFI_AP);
  if (_apPassword.length() >= 8) {
    WiFi.softAP(_apSsid.c_str(), _apPassword.c_str());
  } else {
    WiFi.softAP(_apSsid.c_str());
  }

  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("WiFiManager: AP IP: %s\n", apIP.toString().c_str());

  // Delete any previous server instance.
  if (_server) {
    delete _server;
    _server = nullptr;
  }
  _server = new WebServer(80);

  // Register HTTP handlers for the captive portal.
  _server->on("/", [this]() { handleRoot(); });
  _server->on("/connect", [this]() { handleConnectPage(); });
  _server->on("/submit", HTTP_POST, [this]() { handleSubmitCredentials(); });

  // Setup additional endpoints for captive portal redirection.
  setupCaptivePortal();

  _server->begin();

  // If configured, run the web server on a separate core.
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
  // Check for scan failure (negative value)
  if(n < 0) {
    String errorHtml = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Error</title></head><body>";
    errorHtml += "<h1>Error: WiFi scan failed.</h1>";
    errorHtml += "<p>Please try again later.</p>";
    errorHtml += "</body></html>";
    _server->send(500, "text/html", errorHtml);
    return;
  }

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
    // (Optional) Use std::vector if you want to manipulate the list before output.
    std::vector<String> networks;
    for (int i = 0; i < n; i++) {
      networks.push_back(WiFi.SSID(i));
    }
    for (size_t i = 0; i < networks.size(); i++) {
      // Note: You can also retrieve RSSI directly with WiFi.RSSI(i)
      int32_t rssi = WiFi.RSSI(i);
      html += "<li onclick=\"fillSSID('" + networks[i] + "')\" style='cursor:pointer;'>";
      html += networks[i] + " (" + String(rssi) + " dBm)";
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
  // Validate that an SSID was provided.
  if (!_server->hasArg("ssid") || _server->arg("ssid") == "") {
    _server->send(400, "text/plain", "SSID is required.");
    return;
  }
  String ssid = _server->arg("ssid");
  String password = _server->arg("password");

  // Save the submitted credentials in a thread‐safe way.
  if (xSemaphoreTake(_pendingMutex, portMAX_DELAY) == pdTRUE) {
    _pendingSsid = ssid;
    _pendingPassword = password;
    _newCredentialsAvailable = true;
    xSemaphoreGive(_pendingMutex);
  }

  // Immediately respond to the client.
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Connecting</title></head><body>";
  html += "<h1>Attempting to connect...</h1>";
  html += "<p>Please wait while the connection is attempted.</p>";
  html += "</body></html>";
  _server->send(200, "text/html", html);
}

////////////////////////////////////////////////////////////
// Manager Task (Handles initial connection and new credentials)
////////////////////////////////////////////////////////////
void WiFiManager::managerTask(void* param) {
  WiFiManager* manager = static_cast<WiFiManager*>(param);
  String storedSsid, storedPassword;

  // Attempt connection using stored credentials (if available).
  if (manager->loadLastCredentials(storedSsid, storedPassword)) {
    if (xSemaphoreTake(manager->_statusMutex, portMAX_DELAY) == pdTRUE) {
      manager->_status = WiFiStatus::TRYING_TO_CONNECT;
      xSemaphoreGive(manager->_statusMutex);
    }
    if (manager->tryConnect(storedSsid, storedPassword)) {
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

  // Since stored credentials did not work, ensure AP mode is active for user configuration.
  manager->ensureAPModeActive();

  // Main loop: Poll for new credentials submitted via the captive portal.
  while (true) {
    // Exit loop if WiFi is connected.
    if (WiFi.status() == WL_CONNECTED) {
      if (xSemaphoreTake(manager->_statusMutex, portMAX_DELAY) == pdTRUE) {
        manager->_status = WiFiStatus::CONNECTED;
        xSemaphoreGive(manager->_statusMutex);
      }
      Serial.println("WiFiManager: WiFi connection established.");
      manager->stopAPMode();
      break;
    }

    // Check for new credentials in a thread-safe manner.
    bool newCreds = false;
    String newSsid, newPassword;
    if (xSemaphoreTake(manager->_pendingMutex, portMAX_DELAY) == pdTRUE) {
      if (manager->_newCredentialsAvailable) {
        newCreds = true;
        newSsid = manager->_pendingSsid;
        newPassword = manager->_pendingPassword;
        manager->_newCredentialsAvailable = false;
      }
      xSemaphoreGive(manager->_pendingMutex);
    }

    if (newCreds) {
      // Ensure AP mode is active before attempting connection.
      manager->ensureAPModeActive();
      if (xSemaphoreTake(manager->_statusMutex, portMAX_DELAY) == pdTRUE) {
        manager->_status = WiFiStatus::TRYING_TO_CONNECT;
        xSemaphoreGive(manager->_statusMutex);
      }
      Serial.printf("WiFiManager: Attempting connection with new credentials: '%s'\n", newSsid.c_str());
      if (manager->tryConnect(newSsid, newPassword)) {
        if (xSemaphoreTake(manager->_statusMutex, portMAX_DELAY) == pdTRUE) {
          manager->_status = WiFiStatus::CONNECTED;
          xSemaphoreGive(manager->_statusMutex);
        }
        Serial.println("WiFiManager: Connected using new credentials.");
        manager->stopAPMode();
        break;
      } else {
        Serial.println("WiFiManager: New credentials failed; remaining in AP mode.");
        manager->ensureAPModeActive();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }

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
// Monitor Task (Automatic Reconnection Logic)
////////////////////////////////////////////////////////////
void WiFiManager::monitorTask(void* param) {
  WiFiManager* manager = static_cast<WiFiManager*>(param);
  for (;;) {
    // Only attempt reconnection if in STA mode (i.e. not in AP mode).
    if (WiFi.getMode() == WIFI_STA && WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFiManager: Detected WiFi disconnection. Attempting automatic reconnection...");
      String storedSsid, storedPassword;
      if (manager->loadLastCredentials(storedSsid, storedPassword)) {
        bool reconnected = false;
        // Try up to 3 reconnection attempts.
        for (int attempt = 0; attempt < 3 && !reconnected; attempt++) {
          Serial.printf("WiFiManager: Reconnection attempt %d...\n", attempt + 1);
          reconnected = manager->tryConnect(storedSsid, storedPassword);
          if (!reconnected) {
            vTaskDelay(pdMS_TO_TICKS(3000));
          }
        }
        if (!reconnected) {
          Serial.println("WiFiManager: Automatic reconnection failed, switching to AP mode");
          manager->ensureAPModeActive();
        } else {
          Serial.println("WiFiManager: Reconnected successfully");
        }
      } else {
        Serial.println("WiFiManager: No stored credentials available for reconnection. Switching to AP mode.");
        manager->ensureAPModeActive();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5000)); // Check every 5 seconds.
  }
}

////////////////////////////////////////////////////////////
// Ensure AP Mode is Active for User Configuration
////////////////////////////////////////////////////////////
void WiFiManager::ensureAPModeActive() {
  // If already in AP mode with an active web server, nothing to do.
  if (WiFi.getMode() == WIFI_AP && _server != nullptr) {
    return;
  }
  startAPMode();
  if (xSemaphoreTake(_statusMutex, portMAX_DELAY) == pdTRUE) {
    _status = WiFiStatus::AP_MODE_ACTIVE;
    xSemaphoreGive(_statusMutex);
  }
}
