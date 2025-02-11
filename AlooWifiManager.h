#ifndef ALOO_WIFI_MANAGER_H
#define ALOO_WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

//========================================================================
// WiFi Status Enumeration
//========================================================================
enum class WiFiStatus {
  INITIALIZING,       // Manager task just started
  TRYING_TO_CONNECT,  // Actively trying to connect (with stored or submitted credentials)
  AP_MODE_ACTIVE,     // AP mode is active and captive portal is available
  CONNECTED           // Successfully connected to a WiFi network
};

//========================================================================
// WiFiManager Class Declaration
//========================================================================
class WiFiManager {
public:
  /**
   * @brief Constructor with configurable AP credentials
   * 
   * @param apSsid SSID for the configuration access point
   * @param apPassword Password for the configuration AP (empty string for open network)
   */
  WiFiManager(const String& apSsid = "ESP32-Config", const String& apPassword = "");
  ~WiFiManager();

  /**
   * @brief Starts the asynchronous WiFi management
   * 
   * @param runServerOnSeparateCore Run web server in separate FreeRTOS task
   * @param serverCore CPU core for web server task
   * @param managerCore CPU core for manager task
   */
  void begin(bool runServerOnSeparateCore = true, int serverCore = 1, int managerCore = 1);

  WiFiStatus getStatus();
  void processWebServer();
  bool resetCredentials();

private:
  //========================================================================
  // Private Members
  //========================================================================
  // AP Configuration
  String _apSsid;
  String _apPassword;

  // Connection status and synchronization
  WiFiStatus _status;
  SemaphoreHandle_t _statusMutex;

  // Credential management
  String _pendingSsid;
  String _pendingPassword;
  bool _newCredentialsAvailable;
  SemaphoreHandle_t _pendingMutex;

  // Web server and DNS components
  WebServer* _server;
  DNSServer _dnsServer;
  bool _runServerOnSeparateCore;

  // Task management
  TaskHandle_t _managerTaskHandle;
  TaskHandle_t _serverTaskHandle;
  int _serverCore;
  int _managerCore;

  // Persistent storage
  Preferences _preferences;
  static constexpr char PREF_NAMESPACE[] = "wifimanager";
  static constexpr char PREF_SSID_KEY[] = "last_ssid";
  static constexpr char PREF_PASS_KEY[] = "last_pass";

  //========================================================================
  // Private Methods
  //========================================================================
  void ensureAPModeActive();
  // Credential storage
  bool loadLastCredentials(String &ssid, String &password);
  bool saveLastCredentials(const String &ssid, const String &password);

  // Connection management
  bool tryConnect(const String &ssid, const String &password);
  void startAPMode();
  void stopAPMode();

  // Captive portal functionality
  void setupCaptivePortal();
  void handleRedirect();
  bool isIp(const String& str);

  // Web server handlers
  void handleRoot();
  void handleConnectPage();
  void handleSubmitCredentials();

  // Task functions
  static void managerTask(void* param);
  static void serverTask(void* param);
};

#endif // ALOO_WIFI_MANAGER_H