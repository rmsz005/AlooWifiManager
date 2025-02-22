#ifndef ALOO_WIFI_MANAGER_H
#define ALOO_WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

//========================================================================
// WiFi Status Enumeration
//========================================================================
enum class WiFiStatus {
  INITIALIZING,       // Manager task just started
  TRYING_TO_CONNECT,  // Actively trying to connect (stored or submitted credentials)
  AP_MODE_ACTIVE,     // AP mode active and captive portal available
  CONNECTED,          // Successfully connected to a WiFi network
  DISCONNECTED,       // WiFi is disconnected
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
   * @brief Constructor with configurable AP credentials and optional parameters.
   * @param apSsid SSID for the configuration access point.
   * @param apPassword Password for the configuration AP (empty string for open network).
   * @param autoLaunchAP When true, automatically launch AP mode after a failed connection attempt.
   * @param reconnectionAttempts Number of reconnection attempts before giving up.
   */
  WiFiManager(const String& apSsid = "ESP32-Config", 
              const String& apPassword = "", 
              bool autoLaunchAP = true,
              int reconnectionAttempts = 1);
  ~WiFiManager();

  /**
   * @brief Starts the asynchronous WiFi management and web server.
   *
   * @param runServerOnSeparateCore Run web server in a separate FreeRTOS task.
   * @param serverCore CPU core for web server task.
   * @param managerCore CPU core for manager and monitor tasks.
   * @param managerTaskDelay Delay (in ms) between iterations in the manager task loop.
   * @param serverTaskDelay Delay (in ms) between iterations in the server task loop.
   * @param monitorTaskDelay Delay (in ms) between iterations in the monitor task loop.
   * @param scanTaskDelay Delay (in ms) between iterations in the scan task loop.
   */
  void begin(bool runServerOnSeparateCore = true, int serverCore = 1, int managerCore = 1,
             uint32_t managerTaskDelay = 500, uint32_t serverTaskDelay = 10,
             uint32_t monitorTaskDelay = 5000, uint32_t scanTaskDelay = 15000);

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

  /**
   * @brief Initiates a connection attempt in a non-blocking, event-based way.
   * @param ssid The WiFi SSID.
   * @param password The WiFi password.
   * @return Always returns true (connection result is notified via events).
   */
  bool tryConnect(const String &ssid, const String &password);

private:
  //========================================================================
  // Private Members (Configuration, State, and Tasks)
  //========================================================================
  String _apSsid;
  String _apPassword;

  WiFiStatus _status;
  SemaphoreHandle_t _statusMutex;

  // Credential management for pending credentials submitted via captive portal
  String _pendingSsid;
  String _pendingPassword;
  bool _newCredentialsAvailable;
  SemaphoreHandle_t _pendingMutex;

  // Web server and DNS components
  WebServer* _server;
  DNSServer _dnsServer;
  bool _runServerOnSeparateCore;

  // Task handles and core assignments
  TaskHandle_t _managerTaskHandle;
  TaskHandle_t _serverTaskHandle;
  TaskHandle_t _monitorTaskHandle;
  TaskHandle_t _scanTaskHandle;
  int _serverCore;
  int _managerCore;

  // Persistent storage (using Preferences)
  Preferences _preferences;
  static const char PREF_NAMESPACE[];  // Defined in cpp
  static const char PREF_SSID_KEY[];     // Defined in cpp
  static const char PREF_PASS_KEY[];     // Defined in cpp

  // Mutex to protect simultaneous connection attempts
  SemaphoreHandle_t _connectionMutex;

  // Connection timeout (milliseconds)
  unsigned long _connectTimeout;

  // Cached WiFi networks (for /wifinetworks endpoint)
  std::vector<WiFiNetwork> _cachedNetworks;
  SemaphoreHandle_t _networksMutex;

  //========================================================================
  // Endpoints and Polling Constants
  //========================================================================
  static const char STATUS_ENDPOINT[];   // Defined in cpp
  static constexpr char NETWORKS_ENDPOINT[] = "/wifinetworks";
  static constexpr char SUBMIT_ENDPOINT[] = "/submit";

  //========================================================================
  // Event-based Enhancements
  //========================================================================
  static WiFiManager* _instance;          // Singleton instance for event callbacks
  TimerHandle_t _internetCheckTimer;        // Timer to periodically check internet access
  bool _isConnecting;                       // Flag to indicate ongoing connection attempt
  SemaphoreHandle_t _connectingMutex;       // Mutex to protect _isConnecting

  // New optional parameters
  bool _autoLaunchAP;                       // Whether to automatically launch AP after failed connection
  int _reconnectionAttempts;                // Number of reconnection attempts before giving up

  // New members to store current credentials for saving on successful connection
  String _currentSsid;
  String _currentPassword;

  //========================================================================
  // Task Frequency Parameters (in milliseconds)
  //========================================================================
  uint32_t _managerTaskDelay;
  uint32_t _serverTaskDelay;
  uint32_t _monitorTaskDelay;
  uint32_t _scanTaskDelay;

  //========================================================================
  // Private Helper Functions for Shared Variables and Operations
  //========================================================================
  void updateStatus(WiFiStatus newStatus);
  WiFiStatus safeGetStatus();
  void setPendingCredentials(const String& ssid, const String& password);
  bool fetchPendingCredentials(String &ssid, String &password);

  //========================================================================
  // Web Server Helpers (Default Embedded Web Files)
  //========================================================================
  void setupDefaultEndpoints();

  //========================================================================
  // Credential Storage Helpers
  //========================================================================
  bool loadLastCredentials(String &ssid, String &password);
  bool saveLastCredentials(const String &ssid, const String &password);

  //========================================================================
  // AP Mode and Captive Portal Functions
  //========================================================================
  void startAPMode();
  void stopAPMode();
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
  void ensureAPModeActive();

  //========================================================================
  // Private Helper for Converting WiFiStatus to String
  //========================================================================
  const char* wifiStatusToString(WiFiStatus status);

  //========================================================================
  // Event-based WiFi Event Handler
  //========================================================================
  static void wifiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info);
};

#endif // ALOO_WIFI_MANAGER_H
