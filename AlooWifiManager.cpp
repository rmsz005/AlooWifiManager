#include "AlooWifiManager.h"
#include <DNSServer.h>
#include <HTTPClient.h>

// Provide out-of-line definitions for the static constant members
const char WiFiManager::PREF_NAMESPACE[] = "wifimanager";
const char WiFiManager::PREF_SSID_KEY[] = "last_ssid";
const char WiFiManager::PREF_PASS_KEY[] = "last_pass";
const char WiFiManager::STATUS_ENDPOINT[] = "/status";

// Define the static instance pointer
WiFiManager* WiFiManager::_instance = nullptr;

//--------------------------------------------------------------------------
// Default Embedded Web Files (Fallbacks)
//--------------------------------------------------------------------------
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

static const char connectingHtml[] PROGMEM = R"raw(
<!DOCTYPE html>
<html>
<head>
  <meta charset='UTF-8'>
  <title>Connecting</title>
  <script>
    function checkStatus() {
      fetch('/status')
        .then(response => response.ok ? response.json() : Promise.reject())
        .then(data => {
          if (data.status === 'CONNECTED') {
            window.location.href = '/';
          } else if (data.status === 'AP_MODE_ACTIVE' || data.status === 'DISCONNECTED') {
            alert('Connection failed! Please check credentials.');
            window.location.href = '/connect';
          } else {
            setTimeout(checkStatus, 2000);
          }
        })
        .catch(() => setTimeout(checkStatus, 2000));
    }
    document.addEventListener('DOMContentLoaded', () => setTimeout(checkStatus, 4000));
  </script>
</head>
<body>
  <h1>Attempting to connect...</h1>
  <p>Please wait while we try to connect to %s</p>
  <p>This may take up to %lu seconds</p>
</body>
</html>
)raw";

//--------------------------------------------------------------------------
// Constructor & Destructor
//--------------------------------------------------------------------------
WiFiManager::WiFiManager(const String& apSsid, const String& apPassword, const String& webDir,
                         bool autoLaunchAP, int reconnectionAttempts)
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
    _connectTimeout(15000), // Default 15 seconds
    _isConnecting(false),
    _autoLaunchAP(autoLaunchAP),
    _reconnectionAttempts(reconnectionAttempts)
{
  // Create mutexes for thread safety.
  _statusMutex     = xSemaphoreCreateMutex();
  _pendingMutex    = xSemaphoreCreateMutex();
  _connectionMutex = xSemaphoreCreateMutex();
  _networksMutex   = xSemaphoreCreateMutex();
  _connectingMutex = xSemaphoreCreateMutex();

  // Set the singleton instance and register the WiFi event handler.
  _instance = this;
  WiFi.onEvent(WiFiManager::wifiEventHandler);
}

WiFiManager::~WiFiManager() {
  if (_managerTaskHandle) vTaskDelete(_managerTaskHandle);
  if (_serverTaskHandle) vTaskDelete(_serverTaskHandle);
  if (_monitorTaskHandle) vTaskDelete(_monitorTaskHandle);
  if (_scanTaskHandle) vTaskDelete(_scanTaskHandle);
  if (_internetCheckTimer) xTimerDelete(_internetCheckTimer, 0);
  if (_statusMutex)    vSemaphoreDelete(_statusMutex);
  if (_pendingMutex)   vSemaphoreDelete(_pendingMutex);
  if (_connectionMutex) vSemaphoreDelete(_connectionMutex);
  if (_connectingMutex) vSemaphoreDelete(_connectingMutex);
  if (_networksMutex)  vSemaphoreDelete(_networksMutex);
  stopAPMode();
  if (_instance == this) _instance = nullptr;
}

//--------------------------------------------------------------------------
// Helper Functions for Shared Variables
//--------------------------------------------------------------------------

void WiFiManager::updateStatus(WiFiStatus newStatus) {
  xSemaphoreTake(_statusMutex, portMAX_DELAY);
  if (_status != newStatus) {
    Serial.printf("[WM] Status: %s -> %s\n", wifiStatusToString(_status), wifiStatusToString(newStatus));
  }
  _status = newStatus;
  xSemaphoreGive(_statusMutex);

  // Manage internet check timer based on connection status.
  if (newStatus == WiFiStatus::CONNECTED) {
    if (_internetCheckTimer && xTimerIsTimerActive(_internetCheckTimer) == pdFALSE)
      xTimerStart(_internetCheckTimer, 0);
  } else {
    if (_internetCheckTimer)
      xTimerStop(_internetCheckTimer, 0);
  }
}

WiFiStatus WiFiManager::safeGetStatus() {
  WiFiStatus stat;
  xSemaphoreTake(_statusMutex, portMAX_DELAY);
  stat = _status;
  xSemaphoreGive(_statusMutex);
  return stat;
}

void WiFiManager::setPendingCredentials(const String& ssid, const String& password) {
  xSemaphoreTake(_pendingMutex, portMAX_DELAY);
  _pendingSsid = ssid;
  _pendingPassword = password;
  _newCredentialsAvailable = true;
  xSemaphoreGive(_pendingMutex);
}

bool WiFiManager::fetchPendingCredentials(String &ssid, String &password) {
  bool newCred = false;
  xSemaphoreTake(_pendingMutex, portMAX_DELAY);
  if (_newCredentialsAvailable) {
    ssid = _pendingSsid;
    password = _pendingPassword;
    _newCredentialsAvailable = false;
    newCred = true;
  }
  xSemaphoreGive(_pendingMutex);
  return newCred;
}

//--------------------------------------------------------------------------
// Public API Methods
//--------------------------------------------------------------------------

void WiFiManager::begin(bool runServerOnSeparateCore, int serverCore, int managerCore,
                          uint32_t managerTaskDelay, uint32_t serverTaskDelay,
                          uint32_t monitorTaskDelay, uint32_t scanTaskDelay) {
  _runServerOnSeparateCore = runServerOnSeparateCore;
  _serverCore = serverCore;
  _managerCore = managerCore;

  // Save task frequency parameters.
  _managerTaskDelay = managerTaskDelay;
  _serverTaskDelay = serverTaskDelay;
  _monitorTaskDelay = monitorTaskDelay;
  _scanTaskDelay = scanTaskDelay;

  // Initialize SPIFFS if a web directory is specified.
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
  return safeGetStatus();
}

void WiFiManager::processWebServer() {
  if (!_runServerOnSeparateCore && _server) {
    _server->handleClient();
    _dnsServer.processNextRequest();
  }
}

void WiFiManager::setConnectTimeout(unsigned long timeout) {
  _connectTimeout = timeout;
}

/**
 * @brief Forces the device to start AP mode so that new credentials can be entered.
 */
void WiFiManager::forceAPMode() {
  Serial.println("WiFiManager: Forcing AP mode for new credentials...");
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true);
  stopAPMode();
  startAPMode();
  updateStatus(WiFiStatus::AP_MODE_ACTIVE);
}

/**
 * @brief Initiates a connection attempt using the given credentials.
 *        This is non-blocking; the result is handled via events.
 */
bool WiFiManager::tryConnect(const String &ssid, const String &password) {
  if (xSemaphoreTake(_connectingMutex, portMAX_DELAY) != pdTRUE) return false;
  
  // Save the credentials for later storage upon successful connection.
  _currentSsid = ssid;
  _currentPassword = password;

  updateStatus(WiFiStatus::TRYING_TO_CONNECT);
  Serial.printf("WiFiManager: Attempting to connect to %s   %s\n", ssid.c_str(), password.c_str());
  WiFi.begin(ssid.c_str(), password.c_str());
  
  // Brief delay to allow the connection attempt to start.
  vTaskDelay(pdMS_TO_TICKS(100));
  xSemaphoreGive(_connectingMutex);
  return true;
}

//--------------------------------------------------------------------------
// SPIFFS and File Serving Helpers
//--------------------------------------------------------------------------

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

//--------------------------------------------------------------------------
// Credential Storage Helpers
//--------------------------------------------------------------------------

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

//--------------------------------------------------------------------------
// AP Mode & Captive Portal Functions
//--------------------------------------------------------------------------

void WiFiManager::setupCaptivePortal() {
  // Start DNS server to catch all DNS requests and redirect to the AP IP.
  _dnsServer.start(53, "*", WiFi.softAPIP());
  // Redirect common captive portal requests.
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
    if (_server) {
      Serial.println("WiFiManager: AP mode already active.");
      return;
    }
  }

  Serial.println("WiFiManager: Starting AP mode for WiFi setup...");
  WiFi.disconnect(true);
  delay(100);
  // Use AP+STA mode so that WiFi scanning is allowed.
  WiFi.mode(WIFI_AP_STA);
  if (_apPassword.length() >= 8) {
    WiFi.softAP(_apSsid.c_str(), _apPassword.c_str());
  } else {
    WiFi.softAP(_apSsid.c_str());
  }

  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("WiFiManager: AP IP: %s\n", apIP.toString().c_str());

  if (_server) {
    delete _server;
    _server = nullptr;
  }
  _server = new WebServer(80);

  // Setup static endpoints for serving files.
  setupStaticEndpoint("/", "index.html", defaultIndexHtml, "text/html");
  setupStaticEndpoint("/index.html", "index.html", defaultIndexHtml, "text/html");
  setupStaticEndpoint("/connect", "connect.html", defaultConnectHtml, "text/html");
  setupStaticEndpoint("/style.css", "style.css", defaultStyleCss, "text/css");
  setupStaticEndpoint("/script.js", "script.js", defaultScriptJs, "application/javascript");

  // Endpoint to return cached WiFi networks as JSON.
  _server->on("/wifinetworks", [this]() { handleWifiNetworks(); });
  // Status endpoint to return current status as JSON.
  _server->on(STATUS_ENDPOINT, [this]() {
    static constexpr char jsonTemplate[] = R"({"status":"%s"})";
    char response[sizeof(jsonTemplate) + 20];
    const char* statusStr = [this]() -> const char* {
      switch(safeGetStatus()) {
        case WiFiStatus::INITIALIZING: return "INITIALIZING";
        case WiFiStatus::TRYING_TO_CONNECT: return "TRYING_TO_CONNECT";
        case WiFiStatus::AP_MODE_ACTIVE: return "AP_MODE_ACTIVE";
        case WiFiStatus::CONNECTED: return "CONNECTED";
        case WiFiStatus::DISCONNECTED: return "DISCONNECTED";
        default: return "UNKNOWN";
      }
    }();
    snprintf_P(response, sizeof(response), jsonTemplate, statusStr);
    _server->send(200, "application/json", response);
  });
  // Endpoint for submitting WiFi credentials.
  _server->on("/submit", HTTP_POST, [this]() { handleSubmitCredentials(); });
  // Setup captive portal redirection endpoints.
  setupCaptivePortal();
  _server->begin();

  // Optionally, run the web server on a separate core.
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

  // Create scan task to update available WiFi networks periodically.
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

  // Stop the server and scan tasks to prevent resource conflicts.
  if (_serverTaskHandle) {
    Serial.printf("[DEBUG] Deleting server task: %p\n", _serverTaskHandle);
    vTaskDelete(_serverTaskHandle);
    _serverTaskHandle = nullptr;
  }

  if (_scanTaskHandle) {
    Serial.printf("[DEBUG] Deleting scan task: %p\n", _scanTaskHandle);
    vTaskDelete(_scanTaskHandle);
    _scanTaskHandle = nullptr;
  }

  _dnsServer.stop();
  if (_server) {
    Serial.println("WiFiManager: Stopping web server");
    _server->stop();
    delete _server;
    _server = nullptr;
  }

  if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    WiFi.softAPdisconnect(true);
  }
}

//--------------------------------------------------------------------------
// Web Server HTTP Handlers
//--------------------------------------------------------------------------

void WiFiManager::handleSubmitCredentials() {
  if (!_server->hasArg("ssid") || _server->arg("ssid").isEmpty()) {
    _server->send(400, "text/plain", F("SSID is required"));
    return;
  }
  // Save submitted credentials.
  setPendingCredentials(_server->arg("ssid"), _server->arg("password"));

  char html[sizeof(connectingHtml) + 64];
  snprintf_P(html, sizeof(html), connectingHtml,
             _server->arg("ssid").c_str(), _connectTimeout / 1000);
  _server->send(200, "text/html", html);
}

void WiFiManager::handleWifiNetworks() {
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

//--------------------------------------------------------------------------
// Task Functions
//--------------------------------------------------------------------------

void WiFiManager::managerTask(void* param) {
  WiFiManager* manager = static_cast<WiFiManager*>(param);
  String storedSsid, storedPassword, newSsid, newPassword;

  // Try stored credentials first.
  if (manager->loadLastCredentials(storedSsid, storedPassword)) {
    manager->tryConnect(storedSsid, storedPassword);
    uint32_t startTime = xTaskGetTickCount();
    bool connected = false;
    while (xTaskGetTickCount() - startTime < pdMS_TO_TICKS(manager->_connectTimeout)) {
      if (manager->safeGetStatus() == WiFiStatus::CONNECTED) {
        connected = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (connected) {
      Serial.println("WiFiManager: Connected using stored credentials.");
      manager->stopAPMode();
      vTaskDelete(NULL);
      return;
    } else {
      Serial.println("WiFiManager: Stored credentials failed within timeout.");
    }
  } else {
    Serial.println("WiFiManager: No stored credentials found.");
  }

  // Activate AP mode for new credentials.
  manager->ensureAPModeActive();

  while (true) {
    if (manager->fetchPendingCredentials(newSsid, newPassword)) {
      manager->tryConnect(newSsid, newPassword);
      uint32_t startTime = xTaskGetTickCount();
      bool connected = false;
      while (xTaskGetTickCount() - startTime < pdMS_TO_TICKS(manager->_connectTimeout)) {
        if (manager->safeGetStatus() == WiFiStatus::CONNECTED) {
          connected = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      if (connected) {
        Serial.println("WiFiManager: Connected using new credentials.");
        manager->stopAPMode();
        break;
      } else {
        Serial.println("WiFiManager: New credentials failed within timeout.");
        manager->ensureAPModeActive();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(manager->_managerTaskDelay));
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
    vTaskDelay(pdMS_TO_TICKS(manager->_serverTaskDelay));
  }
}

void WiFiManager::monitorTask(void* param) {
  WiFiManager* manager = static_cast<WiFiManager*>(param);
  for (;;) {
    Serial.printf("WiFiManager monitorTask: Current status: %s\n", manager->wifiStatusToString(manager->safeGetStatus()));
    if (manager->safeGetStatus() == WiFiStatus::AP_MODE_ACTIVE) {
      if (WiFi.getMode() != WIFI_AP_STA) {
        Serial.println("FATAL WiFiManager: AP mode is active but not in AP+STA mode.");
      }
      String storedSsid, storedPassword;
      if (_instance->loadLastCredentials(storedSsid, storedPassword)) {
        _instance->tryConnect(storedSsid, storedPassword);
      }
    }
    if (manager->safeGetStatus() == WiFiStatus::CONNECTED && !manager->hasInternetAccess()) {
      manager->updateStatus(WiFiStatus::NO_INTERNET);
    }
    if (manager->safeGetStatus() == WiFiStatus::NO_INTERNET && manager->hasInternetAccess()) {
      Serial.println("WiFiManager: Internet access restored.");
      manager->updateStatus(WiFiStatus::CONNECTED);
    }
    vTaskDelay(pdMS_TO_TICKS(manager->_monitorTaskDelay));
  }
}

void WiFiManager::scanTask(void* param) {
  WiFiManager* manager = static_cast<WiFiManager*>(param);
  for (;;) {
    int ret = WiFi.scanNetworks(true);
    while (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    int n = WiFi.scanComplete();
    if (n >= 0) {
      std::vector<WiFiNetwork> tempNetworks;
      for (int i = 0; i < n; i++) {
        WiFiNetwork net;
        net.ssid = WiFi.SSID(i);
        net.rssi = WiFi.RSSI(i);
        tempNetworks.push_back(net);
      }
      if (xSemaphoreTake(manager->_networksMutex, portMAX_DELAY) == pdTRUE) {
        manager->_cachedNetworks = tempNetworks;
        xSemaphoreGive(manager->_networksMutex);
      }
    }
    WiFi.scanDelete();
    vTaskDelay(pdMS_TO_TICKS(manager->_scanTaskDelay));
  }
}

bool WiFiManager::hasInternetAccess() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  bool connected = client.connect(IPAddress(1, 1, 1, 1), 80, 3000);
  client.stop();
  return connected;
}

void WiFiManager::ensureAPModeActive() {
  if (safeGetStatus() != WiFiStatus::AP_MODE_ACTIVE || WiFi.getMode() != WIFI_AP_STA || !_server) {
    startAPMode();
    updateStatus(WiFiStatus::AP_MODE_ACTIVE);
  }
}

const char* WiFiManager::wifiStatusToString(WiFiStatus status) {
  switch(status) {
    case WiFiStatus::INITIALIZING: return "INITIALIZING";
    case WiFiStatus::TRYING_TO_CONNECT: return "TRYING_TO_CONNECT";
    case WiFiStatus::AP_MODE_ACTIVE: return "AP_MODE_ACTIVE";
    case WiFiStatus::CONNECTED: return "CONNECTED";
    case WiFiStatus::DISCONNECTED: return "DISCONNECTED";
    case WiFiStatus::NO_INTERNET: return "NO_INTERNET";
    default: return "UNKNOWN";
  }
}

//--------------------------------------------------------------------------
// Event-based WiFi Event Handler
//--------------------------------------------------------------------------

void WiFiManager::wifiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (!_instance) return;

  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("WiFiManager Callback: Connected ARDUINO_EVENT_WIFI_STA_GOT_IP %s\n", WiFi.SSID().c_str());
      _instance->updateStatus(WiFiStatus::CONNECTED);
      _instance->saveLastCredentials(_instance->_currentSsid, _instance->_currentPassword);
      _instance->stopAPMode();
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      uint8_t reason = info.wifi_sta_disconnected.reason;
      Serial.printf("WiFiManager Callback: Disconnected from STA (reason %d)\n", reason);
      if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_AUTH_EXPIRE) {
        Serial.println("WiFiManager Callback: Authentication failed. Disabling auto-reconnect.");
        WiFi.setAutoReconnect(false);
      }
      if (_instance->safeGetStatus() != WiFiStatus::AP_MODE_ACTIVE) {
        if (_instance->_autoLaunchAP) {
          Serial.println("WiFiManager: Switching to AP mode.");
          _instance->ensureAPModeActive();
        } else {
          _instance->updateStatus(WiFiStatus::DISCONNECTED);
        }
      }
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("WiFiManager Callback: STA Connected");
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.println("WiFiManager Callback: AP STA Connected");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.println("WiFiManager Callback: AP STA Disconnected");
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.println("WiFiManager Callback: AP STA IP Assigned");
      break;
    case ARDUINO_EVENT_WIFI_SCAN_DONE:
      Serial.println("WiFiManager Callback: Scan Done");
      break;
    default:
      break;
  }
}
