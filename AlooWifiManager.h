#ifndef ALOO_WIFI_MANAGER_H
#define ALOO_WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/*
 * WiFiManager – A WiFi connection and captive portal manager for the ESP32.
 *
 * This library supports external web files stored in SPIFFS.
 * To use external web files:
 * 1. Create a directory (for example, "web") in your project's data folder.
 * 2. Place your index.html, connect.html, style.css, and script.js files in that folder.
 * 3. When instantiating the WiFiManager, pass the directory name as the third parameter.
 * 4. Use the Arduino SPIFFS uploader (or PlatformIO equivalent) to upload the data.
 * If the files are not found, the library falls back to built-in default pages.
 */

//========================================================================
// WiFi Status Enumeration
//========================================================================
enum class WiFiStatus {
  INITIALIZING,       // Manager task just started
  TRYING_TO_CONNECT,  // Actively trying to connect (stored or submitted credentials)
  AP_MODE_ACTIVE,     // AP mode active and captive portal available
  CONNECTED,          // Successfully connected to a WiFi network
  DISCONNECTED,        // WiFi is disconnected
  NO_INTERNET         // Connected to WiFi but no internet access
};

//========================================================================
// WiFiNetwork Struct
//========================================================================
struct WiFiNetwork {
  String ssid;
  int32_t rssi;
};

//========================================================================
// WiFiManager Class Declaration
//========================================================================
class WiFiManager {
public:
  /**
   * @brief Constructor with configurable AP credentials and web directory.
   * @param apSsid SSID for the configuration access point.
   * @param apPassword Password for the configuration AP (empty string for open network).
   * @param webDir Directory in SPIFFS where external web files are stored.
   *               If empty, default embedded web pages will be used.
   */
  WiFiManager(const String& apSsid = "ESP32-Config", const String& apPassword = "", const String& webDir = "");
  ~WiFiManager();

  /**
   * @brief Starts the asynchronous WiFi management and web server.
   * @param runServerOnSeparateCore Run web server in a separate FreeRTOS task.
   * @param serverCore CPU core for web server task.
   * @param managerCore CPU core for manager and monitor tasks.
   */
  void begin(bool runServerOnSeparateCore = true, int serverCore = 1, int managerCore = 1);

  /**
   * @brief Returns the current WiFi connection status.
   */
  WiFiStatus getStatus();

  /**
   * @brief Processes web server client requests (if not running on a separate core).
   */
  void processWebServer();

  /**
   * @brief Resets stored WiFi credentials.
   */
  bool resetCredentials();

  /**
   * @brief Sets the connection timeout duration (in milliseconds) for connection attempts.
   */
  void setConnectTimeout(unsigned long timeout);

  /**
   * @brief Forces the device to start AP mode (to allow new credentials to be submitted).
   */
  void forceAPMode();

private:
  //========================================================================
  // Private Members
  //========================================================================
  // AP Configuration and Web Directory
  String _apSsid;
  String _apPassword;
  String _webDir; // SPIFFS directory where external web files reside

  // WiFi status and synchronization
  WiFiStatus _status;
  SemaphoreHandle_t _statusMutex;

  // Credential management (for new credentials submitted via the captive portal)
  String _pendingSsid;
  String _pendingPassword;
  bool _newCredentialsAvailable;
  SemaphoreHandle_t _pendingMutex;

  // Web server and DNS components
  WebServer* _server;
  DNSServer _dnsServer;
  bool _runServerOnSeparateCore;

  // Task management handles
  TaskHandle_t _managerTaskHandle;
  TaskHandle_t _serverTaskHandle;
  TaskHandle_t _monitorTaskHandle;
  TaskHandle_t _scanTaskHandle; // Task to update available WiFi networks periodically
  int _serverCore;
  int _managerCore;

  // Persistent storage (using Preferences)
  Preferences _preferences;
  static constexpr char PREF_NAMESPACE[] = "wifimanager";
  static constexpr char PREF_SSID_KEY[] = "last_ssid";
  static constexpr char PREF_PASS_KEY[] = "last_pass";

  // Mutex to prevent simultaneous connection attempts
  SemaphoreHandle_t _connectionMutex;

  // Connection timeout for WiFi connection attempts (in milliseconds)
  unsigned long _connectTimeout;

  // Cached WiFi networks (for the /wifinetworks endpoint)
  std::vector<WiFiNetwork> _cachedNetworks;
  SemaphoreHandle_t _networksMutex;

  //========================================================================
  // New Private Constants for Status Polling and Endpoints
  //========================================================================
  static constexpr unsigned long STATUS_POLL_INTERVAL_MS = 2000;
  static inline constexpr char STATUS_ENDPOINT[] = "/status";
  static constexpr char NETWORKS_ENDPOINT[] = "/wifinetworks";
  static constexpr char SUBMIT_ENDPOINT[] = "/submit";

  //========================================================================
  // Private Helper Functions for Shared Variables
  //========================================================================
  /**
   * @brief Updates the internal WiFi status in a thread-safe manner.
   * @param newStatus The new WiFiStatus.
   */
  void updateStatus(WiFiStatus newStatus);

  /**
   * @brief Retrieves the current WiFi status in a thread-safe manner.
   * @return The current WiFiStatus.
   */
  WiFiStatus safeGetStatus();

  /**
   * @brief Sets new pending credentials (submitted via the captive portal) safely.
   * @param ssid The new SSID.
   * @param password The new password.
   */
  void setPendingCredentials(const String& ssid, const String& password);

  /**
   * @brief Checks for and retrieves pending credentials if available.
   * @param ssid (out) The pending SSID.
   * @param password (out) The pending password.
   * @return True if new credentials were available; otherwise false.
   */
  bool fetchPendingCredentials(String &ssid, String &password);

  //========================================================================
  // SPIFFS and File Serving Helpers
  //========================================================================
  /**
   * @brief Loads a file from SPIFFS.
   * @param filePath Full path to the file.
   * @return Contents of the file as a String, or empty String if error.
   */
  String loadFileFromSPIFFS(const String& filePath);

  /**
   * @brief Retrieves file content from SPIFFS if available; otherwise returns the default content.
   * @param fileName Name of the file (relative to webDir).
   * @param defaultContent Default content to use if file not found.
   */
  String getFileContent(const String& fileName, const char* defaultContent);

  /**
   * @brief Sets up a static endpoint that serves a file with the specified content type.
   * @param uri URI endpoint.
   * @param fileName Name of the file to load from SPIFFS (relative to webDir).
   * @param defaultContent Fallback content if the file is not found.
   * @param contentType MIME type.
   */
  void setupStaticEndpoint(const String& uri, const String& fileName, const char* defaultContent, const char* contentType);

  //========================================================================
  // Credential Storage Helpers
  //========================================================================
  bool loadLastCredentials(String &ssid, String &password);
  bool saveLastCredentials(const String &ssid, const String &password);

  //========================================================================
  // Connection Management Helpers
  //========================================================================
  /**
   * @brief Internal function that attempts a connection to the given credentials.
   * @param ssid The WiFi SSID.
   * @param password The WiFi password.
   * @return True if connected; otherwise false.
   */
  bool tryConnectInternal(const String &ssid, const String &password);

  /**
   * @brief Attempts to connect to the given WiFi credentials.
   *        Automatically updates the status to TRYING_TO_CONNECT, CONNECTED, or DISCONNECTED.
   * @param ssid The WiFi SSID.
   * @param password The WiFi password.
   * @return True if connected; otherwise false.
   */
  bool tryConnect(const String &ssid, const String &password);

  void startAPMode();
  void stopAPMode();

  //========================================================================
  // Captive Portal Functionality
  //========================================================================
  void setupCaptivePortal();
  void handleRedirect();
  bool isIp(const String& str);

  //========================================================================
  // Web Server HTTP Handlers
  //========================================================================
  void handleSubmitCredentials();
  void handleWifiNetworks(); // Returns cached WiFi networks as JSON

  //========================================================================
  // Task Functions
  //========================================================================
  static void managerTask(void* param);
  static void serverTask(void* param);
  static void monitorTask(void* param);
  static void scanTask(void* param);
  bool hasInternetAccess();
  /**
   * @brief Ensures AP mode is active.
   */
  void ensureAPModeActive();

  //========================================================================
  // Private Helper for Converting WiFiStatus to String
  //========================================================================
  const char* wifiStatusToString(WiFiStatus status);
};

#endif // ALOO_WIFI_MANAGER_H
