#include "AlooWifiManager.h"
#include <DNSServer.h>
#include <HTTPClient.h>
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
// New Default Connecting HTML (Fallback)
//==========================================================================
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
    document.addEventListener('DOMContentLoaded', () => setTimeout(checkStatus, 2000));
  </script>
</head>
<body>
  <h1>Attempting to connect...</h1>
  <p>Please wait while we try to connect to %s</p>
  <p>This may take up to %lu seconds</p>
</body>
</html>
)raw";

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
  _statusMutex     = xSemaphoreCreateMutex();
  _pendingMutex    = xSemaphoreCreateMutex();
  _connectionMutex = xSemaphoreCreateMutex();
  _networksMutex   = xSemaphoreCreateMutex();
}

WiFiManager::~WiFiManager() {
  if (_managerTaskHandle) vTaskDelete(_managerTaskHandle);
  if (_serverTaskHandle) vTaskDelete(_serverTaskHandle);
  if (_monitorTaskHandle) vTaskDelete(_monitorTaskHandle);
  if (_scanTaskHandle) vTaskDelete(_scanTaskHandle);
  if (_statusMutex)    vSemaphoreDelete(_statusMutex);
  if (_pendingMutex)   vSemaphoreDelete(_pendingMutex);
  if (_connectionMutex) vSemaphoreDelete(_connectionMutex);
  if (_networksMutex)  vSemaphoreDelete(_networksMutex);
  stopAPMode();
}

//==========================================================================
// Helper Functions for Shared Variables
//==========================================================================
void WiFiManager::updateStatus(WiFiStatus newStatus) {
  xSemaphoreTake(_statusMutex, portMAX_DELAY);
  if (_status != newStatus) {
    Serial.printf("[WM] Status: %s -> %s\n",
      wifiStatusToString(_status), wifiStatusToString(newStatus));
  }
  _status = newStatus;
  xSemaphoreGive(_statusMutex);
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
  return safeGetStatus();
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

/**
 * @brief Forces the device to start AP mode so that new credentials can be entered.
 */
void WiFiManager::forceAPMode() {
  Serial.println("WiFiManager: Forcing AP mode for new credentials...");
  stopAPMode();
  startAPMode();
  updateStatus(WiFiStatus::AP_MODE_ACTIVE);
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
// WiFi Connection Helpers
//==========================================================================
bool WiFiManager::tryConnectInternal(const String &ssid, const String &password) {
  if (xSemaphoreTake(_connectionMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("WiFiManager: Connection attempt already in progress.");
    return false;
  }

  // Set WiFi to station mode and disconnect any current connection.
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
    // Removed WiFi.reasonCode() call since it does not exist in ESP32's WiFi library.
    Serial.printf("[WM] Connection failed to %s (RSSI: %d)\n", ssid.c_str(), WiFi.RSSI());
  }

  xSemaphoreGive(_connectionMutex);
  return connected;
}

bool WiFiManager::tryConnect(const String &ssid, const String &password) {
  // Automatically update status to TRYING_TO_CONNECT
  updateStatus(WiFiStatus::TRYING_TO_CONNECT);
  bool connected = tryConnectInternal(ssid, password);
  if (connected) {
    while (!hasInternetAccess()) {
      Serial.println("WiFiManager: Connected to WiFi but no internet access. waiting...");
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
    updateStatus(WiFiStatus::CONNECTED);
  } else {
    updateStatus(WiFiStatus::DISCONNECTED);
  }
  return connected;
}

//==========================================================================
// AP Mode & Captive Portal Functions
//==========================================================================
void WiFiManager::setupCaptivePortal() {
  // Start the DNS server to catch all DNS requests and redirect to the AP IP.
  _dnsServer.start(53, "*", WiFi.softAPIP());

  // Handle redirection for common endpoints used by captive portals.
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

  // Modified status endpoint handler with lambda capturing 'this'
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
  if (!_server->hasArg("ssid") || _server->arg("ssid").isEmpty()) {
    _server->send(400, "text/plain", F("SSID is required"));
    return;
  }

  setPendingCredentials(_server->arg("ssid"), _server->arg("password"));

  char html[sizeof(connectingHtml) + 64]; // Calculate proper size
  snprintf_P(html, sizeof(html), connectingHtml,
            _server->arg("ssid").c_str(), _connectTimeout / 1000);
  
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
  String newSsid, newPassword;
  unsigned long lastAttemptTime = millis();

  // Try to load stored credentials.
  if (manager->loadLastCredentials(storedSsid, storedPassword)) {
    if (manager->tryConnect(storedSsid, storedPassword)) {
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

  // Ensure AP mode is active and update status.
  manager->ensureAPModeActive();

  // Main loop: poll for new credentials and monitor connection status.
  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      manager->updateStatus(WiFiStatus::CONNECTED);
      Serial.println("WiFiManager: WiFi connection established.");
      manager->stopAPMode();
      break;
    }
    // Check for new credentials.
    if (manager->fetchPendingCredentials(newSsid, newPassword)) {
      lastAttemptTime = millis(); // Reset timer on new attempt.
      manager->updateStatus(WiFiStatus::TRYING_TO_CONNECT);
      Serial.printf("WiFiManager: Attempting connection with new credentials: '%s'\n", newSsid.c_str());
      if (manager->tryConnect(newSsid, newPassword)) {
        Serial.println("WiFiManager: Connected using new credentials.");
        manager->updateStatus(WiFiStatus::CONNECTED);
        break;
      } else {
        Serial.println("WiFiManager: New credentials failed; remaining in AP mode.");
        manager->updateStatus(WiFiStatus::DISCONNECTED);
        manager->ensureAPModeActive();
      }
    }
    
    // Handle status timeout
    if (manager->safeGetStatus() == WiFiStatus::TRYING_TO_CONNECT &&
        millis() - lastAttemptTime > manager->_connectTimeout) {
      manager->updateStatus(WiFiStatus::DISCONNECTED);
      Serial.println("WiFiManager: Connection attempt timed out. Switching to AP mode.");
      manager->ensureAPModeActive();
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
    // Check if WiFi is in station mode but not connected.
    if (WiFi.getMode() == WIFI_STA && WiFi.status() != WL_CONNECTED &&
        manager->safeGetStatus() != WiFiStatus::TRYING_TO_CONNECT) {
      Serial.println("WiFiManager: Detected WiFi disconnection. Attempting automatic reconnection...");
      manager->updateStatus(WiFiStatus::DISCONNECTED);
      String storedSsid, storedPassword;
      if (manager->loadLastCredentials(storedSsid, storedPassword)) {
        bool reconnected = false;
        for (int attempt = 0; attempt < 1 && !reconnected; attempt++) {
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
    } else if (manager->safeGetStatus() == WiFiStatus::CONNECTED && !manager->hasInternetAccess()) {
      manager->updateStatus(WiFiStatus::NO_INTERNET);
    }

    if (manager->safeGetStatus() == WiFiStatus::NO_INTERNET && manager->hasInternetAccess()) {
      Serial.println("WiFiManager: Internet access restored.");
      manager->updateStatus(WiFiStatus::CONNECTED);
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

bool WiFiManager::hasInternetAccess() {
  // First, ensure we are connected to WiFi
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  http.setConnectTimeout(3000); // Set a 3-second timeout
  http.begin("http://clients3.google.com/generate_204");
  int httpCode = http.GET();
  http.end();

  // Expecting a 204 No Content response indicates that internet access is available.
  return (httpCode == 204);
}

//==========================================================================
// Helper: Ensure AP Mode is Active
//==========================================================================
void WiFiManager::ensureAPModeActive() {
  if (safeGetStatus() != WiFiStatus::AP_MODE_ACTIVE || 
     WiFi.getMode() != WIFI_AP_STA || !_server) {
    startAPMode();
    updateStatus(WiFiStatus::AP_MODE_ACTIVE);
  }
}

//==========================================================================
// Private Helper for Converting WiFiStatus to String
//==========================================================================
const char* WiFiManager::wifiStatusToString(WiFiStatus status) {
  switch(status) {
    case WiFiStatus::INITIALIZING: return "INITIALIZING";
    case WiFiStatus::TRYING_TO_CONNECT: return "TRYING_TO_CONNECT";
    case WiFiStatus::AP_MODE_ACTIVE: return "AP_MODE_ACTIVE";
    case WiFiStatus::CONNECTED: return "CONNECTED";
    case WiFiStatus::DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}
