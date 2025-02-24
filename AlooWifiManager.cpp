#include "AlooWifiManager.h"
#include <DNSServer.h>
#include <HTTPClient.h>
#ifdef ESP32
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <esp_netif.h>
#endif
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
// Persistent Storage Keys
//--------------------------------------------------------------------------
const char WiFiManager::PREF_NAMESPACE[] = "wifimanager";
const char WiFiManager::PREF_SSID_KEY[] = "last_ssid";
const char WiFiManager::PREF_PASS_KEY[] = "last_pass";
const char WiFiManager::STATUS_ENDPOINT[] = "/status";

//--------------------------------------------------------------------------
// Static Instance Pointer
//--------------------------------------------------------------------------
WiFiManager* WiFiManager::_instance = nullptr;

//--------------------------------------------------------------------------
// Constructor & Destructor
//--------------------------------------------------------------------------

WiFiManager::WiFiManager(const String& apSsid, const String& apPassword, bool autoLaunchAP, int reconnectionAttempts)
  : _apSsid(apSsid),
    _apPassword(apPassword),
    _status(WiFiStatus::INITIALIZING),
    _pendingSsid(""),
    _pendingPassword(""),
    _newCredentialsAvailable(false),
    _server(nullptr),
    _runServerOnSeparateCore(false),
    _connectionManagerTaskHandle(nullptr),
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
  // Create new mutex for WiFi operations
  _wifiMutex = xSemaphoreCreateMutex();

  // Set the singleton instance and register the WiFi event handler.
  _instance = this;
  WiFi.onEvent(WiFiManager::wifiEventHandler);
}

WiFiManager::~WiFiManager() {
  if (_connectionManagerTaskHandle) vTaskDelete(_connectionManagerTaskHandle);
  if (_serverTaskHandle) vTaskDelete(_serverTaskHandle);
  if (_monitorTaskHandle) vTaskDelete(_monitorTaskHandle);
  if (_scanTaskHandle) vTaskDelete(_scanTaskHandle);
  if (_internetCheckTimer) xTimerDelete(_internetCheckTimer, 0);
  if (_statusMutex)    vSemaphoreDelete(_statusMutex);
  if (_pendingMutex)   vSemaphoreDelete(_pendingMutex);
  if (_connectionMutex) vSemaphoreDelete(_connectionMutex);
  if (_connectingMutex) vSemaphoreDelete(_connectingMutex);
  if (_networksMutex)  vSemaphoreDelete(_networksMutex);
  // Delete WiFi mutex
  if (_wifiMutex) vSemaphoreDelete(_wifiMutex);
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

  Serial.println("WiFiManager: Starting asynchronous initialization...");

  // Create the persistent connection manager task.
  BaseType_t result = xTaskCreatePinnedToCore(
    connectionManagerTask,
    "WiFiConnMgrTask",
    8192,
    this,
    1,
    &_connectionManagerTaskHandle,
    _managerCore
  );
  if (result != pdPASS) {
    Serial.println("WiFiManager: Failed to create connection manager task.");
    _connectionManagerTaskHandle = nullptr;
  }

  // Create the monitor task for checking connectivity.
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
  Serial.printf("WiFiManager: Attempting to connect to %s\n", ssid.c_str());
  
  // Use WiFi mutex to ensure exclusive access during connection attempts.
  xSemaphoreTake(_wifiMutex, portMAX_DELAY);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  xSemaphoreGive(_wifiMutex);
  
  // Brief delay to allow the connection attempt to start.
  vTaskDelay(pdMS_TO_TICKS(100));
  xSemaphoreGive(_connectingMutex);
  return true;
}
bool WiFiManager::resetWiFi() {
    xSemaphoreTake(_wifiMutex, portMAX_DELAY);
    
    Serial.println("WiFiManager: Performing full WiFi reset...");
    
    // Force disconnect and disable interfaces
    WiFi.disconnect(true, true);  // Disconnect + disable STA
    WiFi.enableAP(false);
    delay(100);
    
    // Clear persistent settings
    WiFi.persistent(false);
    WiFi.setAutoConnect(false);
    WiFi.setAutoReconnect(false);
    
    // ESP-IDF level cleanup
    esp_wifi_deinit();
    esp_wifi_stop();
    delay(100);
    
    // Reinitialize WiFi with default config
    WiFi.mode(WIFI_MODE_NULL);
    
    // Create and initialize default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    
    xSemaphoreGive(_wifiMutex);
    
    Serial.println("WiFiManager: WiFi stack fully reset");
    return true;
}

//--------------------------------------------------------------------------
// Web Server Helpers (Default Embedded Web Files)
//--------------------------------------------------------------------------
void WiFiManager::setupDefaultEndpoints() {
  // Setup endpoints to serve the default embedded HTML, CSS, and JS files.
  _server->on("/", [this]() {
    _server->send(200, "text/html", defaultIndexHtml);
  });
  _server->on("/index.html", [this]() {
    _server->send(200, "text/html", defaultIndexHtml);
  });
  _server->on("/connect", [this]() {
    _server->send(200, "text/html", defaultConnectHtml);
  });
  _server->on("/style.css", [this]() {
    _server->send(200, "text/css", defaultStyleCss);
  });
  _server->on("/script.js", [this]() {
    _server->send(200, "application/javascript", defaultScriptJs);
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

  // Setup default endpoints to serve embedded web files.
  setupDefaultEndpoints();

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

/**
 * @brief Persistent connection manager task.
 *
 * This task continuously monitors the connection status. If not connected
 * (and not in a NO_INTERNET state), it checks for pending credentials first,
 * then attempts stored credentials (only once per disconnection). The actual
 * connection attempt is done via a helper lambda that calls tryConnect() and
 * waits (non-blocking) for a notification from the WiFi event callback.
 */
void WiFiManager::connectionManagerTask(void* param) {
  WiFiManager* manager = static_cast<WiFiManager*>(param);
  bool attemptedStored = false;
  static const int MAX_RETRIES = 5;
  // Helper lambda for attempting a connection.
  auto attemptConnection = [manager](const String &ssid, const String &password, const char* type) -> bool {
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
      Serial.printf("WiFiManager: Attempt %d to connect with %s credentials: %s\n", attempt + 1, type, ssid.c_str());
      // Ensure autoReconnect is enabled.
      WiFi.disconnect(false, false);
      manager->tryConnect(ssid, password);
      xTaskNotifyStateClear(NULL);
      if (xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(manager->_connectTimeout)) == pdTRUE) {
        if (manager->safeGetStatus() == WiFiStatus::CONNECTED)
          return true;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      Serial.printf("WiFiManager: Attempt %d failed.\n", attempt + 1);
    }
    return false;
  };

  manager->resetWiFi();

  for (;;) {
    // If already connected (or in NO_INTERNET state), reset flag.
    if (manager->safeGetStatus() == WiFiStatus::CONNECTED ||
        manager->safeGetStatus() == WiFiStatus::NO_INTERNET) {
      attemptedStored = false;
      vTaskDelay(pdMS_TO_TICKS(manager->_managerTaskDelay));
      continue;
    }
    // Try pending credentials first.
    String newSsid, newPassword;
    if (manager->fetchPendingCredentials(newSsid, newPassword)) {
      if (!attemptConnection(newSsid, newPassword, "pending")) {
        Serial.println("WiFiManager: Pending credentials connection failed.");
        manager->ensureAPModeActive();
      }
    }
    // Otherwise, try stored credentials if not yet attempted.
    else if (!attemptedStored) {
      String storedSsid, storedPassword;
      if (manager->loadLastCredentials(storedSsid, storedPassword)) {
        if (!attemptConnection(storedSsid, storedPassword, "stored")) {
          Serial.println("WiFiManager: Stored credentials connection failed.");
          manager->ensureAPModeActive();
        }
        attemptedStored = true;
      } else {
        // No stored credentials; force AP mode.
        manager->ensureAPModeActive();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(manager->_managerTaskDelay));
  }
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
    // Only monitor internet connectivity.
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
    // Use WiFi mutex to prevent concurrent operations.
    xSemaphoreTake(manager->_wifiMutex, portMAX_DELAY);
    Serial.println("[WM] Starting WiFi scan...");
    int ret = WiFi.scanNetworks(true);
    if(ret == WIFI_SCAN_RUNNING) {
      Serial.println("[WM] Scan initiated asynchronously.");
    }
    // Clear any previous notifications.
    xTaskNotifyStateClear(NULL);
    // Wait for scan completion notification from the WiFi event handler,
    // with a timeout of 10 seconds.
    BaseType_t notifyResult = xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(10000));
    if (notifyResult == pdTRUE) {
       Serial.println("[WM] Scan notification received.");
    } else {
       Serial.println("[WM] Scan notification timeout.");
    }
    int n = WiFi.scanComplete();
    Serial.printf("[WM] WiFi scan complete, found %d networks.\n", n);
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
    } else {
      Serial.println("[WM] Scan failed or no networks found.");
    }
    WiFi.scanDelete();
    xSemaphoreGive(manager->_wifiMutex);
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
      Serial.printf("WiFiManager Callback: Got IP on SSID %s\n", WiFi.SSID().c_str());
      _instance->updateStatus(WiFiStatus::CONNECTED);
      _instance->saveLastCredentials(_instance->_currentSsid, _instance->_currentPassword);
      _instance->stopAPMode();
      // Notify the connection manager of success.
      if (_instance->_connectionManagerTaskHandle) {
          xTaskNotifyGive(_instance->_connectionManagerTaskHandle);
      }
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      uint8_t reason = info.wifi_sta_disconnected.reason;
      Serial.printf("WiFiManager Callback: Disconnected from STA (reason %d)\n", reason);
      if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_AUTH_EXPIRE) {
        Serial.println("WiFiManager Callback: Authentication failed. Disabling auto-reconnect.");
        // WiFi.setAutoReconnect(false);
      }
      // Notify the connection manager immediately so that waiting attempts wake up.
      if (_instance->_connectionManagerTaskHandle) {
          xTaskNotifyGive(_instance->_connectionManagerTaskHandle);
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
      // Notify scan task that scan is complete.
      if (_instance->_scanTaskHandle) {
          xTaskNotifyGive(_instance->_scanTaskHandle);
      }
      break;
    default:
      break;
  }
}
