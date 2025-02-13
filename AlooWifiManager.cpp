#include "AlooWifiManager.h"
#include <DNSServer.h>

// Initialize static member variables for Preferences keys
constexpr char WiFiManager::PREF_NAMESPACE[];
constexpr char WiFiManager::PREF_SSID_KEY[];
constexpr char WiFiManager::PREF_PASS_KEY[];

//==========================================================================
// Default Embedded Web Files (Fallbacks)
//==========================================================================
static const char defaultIndexHtml[] =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"  <meta charset='UTF-8'>\n"
"  <title>ESP32 WiFi Setup</title>\n"
"  <link rel='stylesheet' href='/style.css'>\n"
"</head>\n"
"<body>\n"
"  <h1>ESP32 WiFi Setup</h1>\n"
"  <button onclick=\"window.location.href='/connect'\">Setup WiFi</button>\n"
"  <script src='/script.js'></script>\n"
"</body>\n"
"</html>";

static const char defaultConnectHtml[] =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"  <meta charset='UTF-8'>\n"
"  <title>Select WiFi Network</title>\n"
"  <link rel='stylesheet' href='/style.css'>\n"
"</head>\n"
"<body>\n"
"  <h1>Select WiFi Network</h1>\n"
"  <div id='networks'></div>\n"
"  <form id='wifiForm' action='/submit' method='POST'>\n"
"    SSID: <input type='text' id='ssid' name='ssid'><br>\n"
"    Password: <input type='password' id='password' name='password'><br>\n"
"    <input type='checkbox' onclick='togglePassword()'> Show Password<br>\n"
"    <input type='submit' value='Connect'>\n"
"  </form>\n"
"  <script src='/script.js'></script>\n"
"</body>\n"
"</html>";

static const char defaultStyleCss[] =
"body { font-family: Arial, sans-serif; background-color: #f2f2f2; text-align: center; }\n"
"h1 { color: #333; }\n"
"button { padding: 10px 20px; font-size: 16px; }\n"
"ul { list-style-type: none; padding: 0; }\n"
"li { padding: 8px; margin: 5px; background-color: #fff; border: 1px solid #ddd; cursor: pointer; }";

static const char defaultScriptJs[] =
"function togglePassword() {\n"
"  var x = document.getElementById('password');\n"
"  if (x.type === 'password') { x.type = 'text'; } else { x.type = 'password'; }\n"
"}\n"
"\n"
"function fetchNetworks() {\n"
"  fetch('/wifinetworks')\n"
"    .then(response => response.json())\n"
"    .then(data => {\n"
"      var networksDiv = document.getElementById('networks');\n"
"      if(data.networks && data.networks.length > 0) {\n"
"        var ul = document.createElement('ul');\n"
"        data.networks.forEach(function(net) {\n"
"          var li = document.createElement('li');\n"
"          li.textContent = net.ssid + ' (' + net.rssi + ' dBm)';\n"
"          li.onclick = function() {\n"
"            document.getElementById('ssid').value = net.ssid;\n"
"            document.getElementById('password').focus();\n"
"          };\n"
"          ul.appendChild(li);\n"
"        });\n"
"        networksDiv.innerHTML = '';\n"
"        networksDiv.appendChild(ul);\n"
"      } else {\n"
"        networksDiv.innerHTML = '<p>No networks found. Please refresh.</p>';\n"
"      }\n"
"    })\n"
"    .catch(err => { console.error('Error fetching networks: ', err); });\n"
"}\n"
"\n"
"if(document.getElementById('networks')) { window.onload = fetchNetworks; }";

//==========================================================================
// Constructor & Destructor
//==========================================================================
WiFiManager::WiFiManager(const String& apSsid, const String& apPassword, const String& webDir)
  : _apSsid(apSsid),
    _apPassword(apPassword),
    _webDir(webDir),
    _status(WiFiStatus::INITIALIZING),
    _pendingSsid(""),
    _pendingPassword(""),
    _newCredentialsAvailable(false),
    _server(nullptr),
    _runServerOnSeparateCore(false),
    _managerTaskHandle(nullptr),
    _serverTaskHandle(nullptr),
    _monitorTaskHandle(nullptr),
    _scanTaskHandle(nullptr),
    _serverCore(1),
    _managerCore(1),
    _connectTimeout(15000) // Default connection timeout: 15 seconds
{
  _statusMutex    = xSemaphoreCreateMutex();
  _pendingMutex   = xSemaphoreCreateMutex();
  _connectionMutex = xSemaphoreCreateMutex();
  _networksMutex  = xSemaphoreCreateMutex();
}

WiFiManager::~WiFiManager() {
  if (_managerTaskHandle) vTaskDelete(_managerTaskHandle);
  if (_serverTaskHandle) vTaskDelete(_serverTaskHandle);
  if (_monitorTaskHandle) vTaskDelete(_monitorTaskHandle);
  if (_scanTaskHandle) vTaskDelete(_scanTaskHandle);
  if (_statusMutex) vSemaphoreDelete(_statusMutex);
  if (_pendingMutex) vSemaphoreDelete(_pendingMutex);
  if (_connectionMutex) vSemaphoreDelete(_connectionMutex);
  if (_networksMutex) vSemaphoreDelete(_networksMutex);
  stopAPMode();
}

//==========================================================================
// Public API Methods
//==========================================================================
void WiFiManager::begin(bool runServerOnSeparateCore, int serverCore, int managerCore) {
  _runServerOnSeparateCore = runServerOnSeparateCore;
  _serverCore = serverCore;
  _managerCore = managerCore;

  // Initialize SPIFFS if a web directory was specified.
  if (!_webDir.isEmpty()) {
    if (!SPIFFS.begin(true)) {
      Serial.println("WiFiManager: SPIFFS Mount Failed");
    }
  }

  Serial.println("WiFiManager: Starting asynchronous initialization...");

  // Create the manager task.
  BaseType_t result = xTaskCreatePinnedToCore(
    managerTask,
    "WiFiManagerTask",
    8192,
    this,
    1,
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
    4096,
    this,
    1,
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

//==========================================================================
// SPIFFS and File Serving Helpers
//==========================================================================
String WiFiManager::loadFileFromSPIFFS(const String& filePath) {
  File file = SPIFFS.open(filePath, "r");
  if (!file || file.isDirectory()) {
    Serial.printf("WiFiManager: Failed to open file: %s\n", filePath.c_str());
    return "";
  }
  String content = file.readString();
  file.close();
  return content;
}

String WiFiManager::getFileContent(const String& fileName, const char* defaultContent) {
  if (!_webDir.isEmpty()) {
    String fullPath = _webDir + "/" + fileName;
    String content = loadFileFromSPIFFS(fullPath);
    if (!content.isEmpty()) {
      return content;
    } else {
      Serial.printf("WiFiManager: File %s not found in SPIFFS, falling back to default.\n", fullPath.c_str());
    }
  }
  return String(defaultContent);
}

void WiFiManager::setupStaticEndpoint(const String& uri, const String& fileName, const char* defaultContent, const char* contentType) {
  _server->on(uri.c_str(), [this, fileName, defaultContent, contentType]() {
    String content = getFileContent(fileName, defaultContent);
    _server->send(200, contentType, content);
  });
}

//==========================================================================
// Credential Storage Helpers
//==========================================================================
bool WiFiManager::loadLastCredentials(String &ssid, String &password) {
  if (!_preferences.begin(PREF_NAMESPACE, true)) {
    Serial.println("WiFiManager: Failed to initialize preferences (read-only).");
    return false;
  }
  ssid = _preferences.getString(PREF_SSID_KEY, "");
  password = _preferences.getString(PREF_PASS_KEY, "");
  _preferences.end();
  return (!ssid.isEmpty() && !password.isEmpty());
}

bool WiFiManager::saveLastCredentials(const String &ssid, const String &password) {
  if (!_preferences.begin(PREF_NAMESPACE, false)) {
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

//==========================================================================
// WiFi Connection Helper
//==========================================================================
bool WiFiManager::tryConnect(const String &ssid, const String &password) {
  if (xSemaphoreTake(_connectionMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("WiFiManager: Connection attempt already in progress.");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  Serial.printf("WiFiManager: Attempting to connect to '%s'\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long startTime = millis();
  bool connected = false;
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

//==========================================================================
// AP Mode & Captive Portal Functions
//==========================================================================
void WiFiManager::setupCaptivePortal() {
  _dnsServer.start(53, "*", WiFi.softAPIP());

  _server->on("/generate_204", [this]() { handleRedirect(); });
  _server->on("/hotspot-detect.html", [this]() { handleRedirect(); });
  _server->on("/connecttest.txt", [this]() { handleRedirect(); });
  _server->on("/ncsi.txt", [this]() { handleRedirect(); });

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
  if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    if (_server) return;
  }

  Serial.println("WiFiManager: Starting AP mode for WiFi setup...");

  // Use AP+STA mode so that scanning is allowed.
  WiFi.mode(WIFI_AP_STA);
  if (_apPassword.length() >= 8) {
    WiFi.softAP(_apSsid.c_str(), _apPassword.c_str());
  } else {
    WiFi.softAP(_apSsid.c_str());
  }

  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("WiFiManager: AP IP: %s\n", apIP.toString().c_str());

  // Initialize the web server.
  if (_server) {
    delete _server;
    _server = nullptr;
  }
  _server = new WebServer(80);

  // Setup endpoints to serve static files.
  setupStaticEndpoint("/", "index.html", defaultIndexHtml, "text/html");
  setupStaticEndpoint("/index.html", "index.html", defaultIndexHtml, "text/html");
  setupStaticEndpoint("/connect", "connect.html", defaultConnectHtml, "text/html");
  setupStaticEndpoint("/style.css", "style.css", defaultStyleCss, "text/css");
  setupStaticEndpoint("/script.js", "script.js", defaultScriptJs, "application/javascript");

  // Endpoint to return cached WiFi networks as JSON.
  _server->on("/wifinetworks", [this]() { handleWifiNetworks(); });

  // Endpoint for submitting WiFi credentials.
  _server->on("/submit", HTTP_POST, [this]() { handleSubmitCredentials(); });

  // Setup captive portal redirection endpoints.
  setupCaptivePortal();

  _server->begin();

  // If the web server is to run on a separate core, create the server task.
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

  // Create the scan task for periodic WiFi network updates.
  if (!_scanTaskHandle) {
    BaseType_t result = xTaskCreatePinnedToCore(
      scanTask,
      "WiFiScanTask",
      4096,
      this,
      1,
      &_scanTaskHandle,
      _managerCore
    );
    if (result != pdPASS) {
      Serial.println("WiFiManager: Failed to create scan task");
      _scanTaskHandle = nullptr;
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
  if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    WiFi.softAPdisconnect(true);
  }
  if (_serverTaskHandle) {
    vTaskDelete(_serverTaskHandle);
    _serverTaskHandle = nullptr;
  }
  if (_scanTaskHandle) {
    vTaskDelete(_scanTaskHandle);
    _scanTaskHandle = nullptr;
  }
}

//==========================================================================
// Web Server HTTP Handlers
//==========================================================================
void WiFiManager::handleSubmitCredentials() {
  if (!_server->hasArg("ssid") || _server->arg("ssid") == "") {
    _server->send(400, "text/plain", "SSID is required.");
    return;
  }
  String ssid = _server->arg("ssid");
  String password = _server->arg("password");

  if (xSemaphoreTake(_pendingMutex, portMAX_DELAY) == pdTRUE) {
    _pendingSsid = ssid;
    _pendingPassword = password;
    _newCredentialsAvailable = true;
    xSemaphoreGive(_pendingMutex);
  }

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Connecting</title></head><body>";
  html += "<h1>Attempting to connect...</h1>";
  html += "<p>Please wait while the connection is attempted.</p>";
  html += "</body></html>";
  _server->send(200, "text/html", html);
}

void WiFiManager::handleWifiNetworks() {
  // Build a JSON response from the cached networks.
  String json = "{ \"networks\": [";
  if (xSemaphoreTake(_networksMutex, portMAX_DELAY) == pdTRUE) {
    for (size_t i = 0; i < _cachedNetworks.size(); i++) {
      json += "{ \"ssid\": \"" + _cachedNetworks[i].ssid + "\", \"rssi\": " + String(_cachedNetworks[i].rssi) + " }";
      if (i < _cachedNetworks.size() - 1) json += ", ";
    }
    xSemaphoreGive(_networksMutex);
  }
  json += "] }";
  _server->send(200, "application/json", json);
}

//==========================================================================
// Task Functions
//==========================================================================
void WiFiManager::managerTask(void* param) {
  WiFiManager* manager = static_cast<WiFiManager*>(param);
  String storedSsid, storedPassword;

  // Try to load stored credentials.
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

  // Instead of calling startAPMode() directly, call ensureAPModeActive()
  // so that the status is updated to AP_MODE_ACTIVE.
  manager->ensureAPModeActive();

  // Main loop: poll for new credentials.
  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      if (xSemaphoreTake(manager->_statusMutex, portMAX_DELAY) == pdTRUE) {
        manager->_status = WiFiStatus::CONNECTED;
        xSemaphoreGive(manager->_statusMutex);
      }
      Serial.println("WiFiManager: WiFi connection established.");
      manager->stopAPMode();
      break;
    }

    // Check for new credentials in a thread-safe way.
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
      // Ensure AP mode is active.
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

void WiFiManager::monitorTask(void* param) {
  WiFiManager* manager = static_cast<WiFiManager*>(param);
  for (;;) {
    if (WiFi.getMode() == WIFI_STA && WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFiManager: Detected WiFi disconnection. Attempting automatic reconnection...");
      String storedSsid, storedPassword;
      if (manager->loadLastCredentials(storedSsid, storedPassword)) {
        bool reconnected = false;
        for (int attempt = 0; attempt < 3 && !reconnected; attempt++) {
          Serial.printf("WiFiManager: Reconnection attempt %d...\n", attempt + 1);
          reconnected = manager->tryConnect(storedSsid, storedPassword);
          if (!reconnected) {
            vTaskDelay(pdMS_TO_TICKS(3000));
          }
        }
        if (!reconnected) {
          Serial.println("WiFiManager: Automatic reconnection failed, switching to AP mode");
          if (WiFi.getMode() != WIFI_AP_STA) manager->startAPMode();
        } else {
          Serial.println("WiFiManager: Reconnected successfully");
        }
      } else {
        Serial.println("WiFiManager: No stored credentials available for reconnection. Switching to AP mode.");
        if (WiFi.getMode() != WIFI_AP_STA) manager->startAPMode();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void WiFiManager::scanTask(void* param) {
  WiFiManager* manager = static_cast<WiFiManager*>(param);
  for (;;) {
    // Only scan if in AP+STA mode.
    if (WiFi.getMode() == WIFI_AP_STA) {
      int n = WiFi.scanNetworks();
      if(n >= 0) {
        std::vector<WiFiNetwork> tempNetworks;
        for (int i = 0; i < n; i++) {
          WiFiNetwork net;
          net.ssid = WiFi.SSID(i);
          net.rssi = WiFi.RSSI(i);
          tempNetworks.push_back(net);
        }
        if(xSemaphoreTake(manager->_networksMutex, portMAX_DELAY) == pdTRUE) {
          manager->_cachedNetworks = tempNetworks;
          xSemaphoreGive(manager->_networksMutex);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(15000)); // Scan every 15 seconds
  }
}

//==========================================================================
// Helper: Ensure AP Mode is Active
//==========================================================================
void WiFiManager::ensureAPModeActive() {
  if (WiFi.getMode() == WIFI_AP_STA && _server != nullptr) {
    return;
  }
  startAPMode();
  if (xSemaphoreTake(_statusMutex, portMAX_DELAY) == pdTRUE) {
    _status = WiFiStatus::AP_MODE_ACTIVE;
    xSemaphoreGive(_statusMutex);
  }
}
