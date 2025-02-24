# AlooWifiManager

AlooWifiManager is an asynchronous WiFi manager library for ESP32 projects built on the Arduino framework. It simplifies WiFi connection management by providing non-blocking, event-driven connection handling with automatic fallback to AP mode and a built-in captive portal for easy configuration.

## Features

- **Asynchronous Connection Management:**  
  Uses multiple FreeRTOS tasks to connect to WiFi, monitor connection status, and scan available networks without blocking your main application.

- **Captive Portal:**  
  Automatically switches to Access Point mode when stored credentials are invalid or absent, serving a customizable web portal for WiFi configuration.

- **Web Server Integration:**  
  Embeds an asynchronous web server that provides endpoints for network scanning (`/wifinetworks`), reporting connection status (`/status`), and accepting new credentials (`/submit`).

- **Persistent Credential Storage:**  
  Utilizes ESP32 Preferences to save and retrieve WiFi credentials and custom parameters across reboots.

- **Customizable Parameters:**  
  Allows configuration of connection timeouts, reconnection attempts, task delays, and other runtime parameters to tailor the behavior for different applications.

## Planned Improvements (TODO)

- **Add Timeouts:**  
  Implement configurable timeouts for WiFi connection attempts and captive portal sessions to improve system responsiveness.

- **ESP8266 Compatibility:**  
  Extend support to the ESP8266 platform ensuring the core functionalities are portable across both ESP32 and ESP8266.

- **Add Event Callbacks:**  
  Provide user-defined callback hooks for events such as successful connection, configuration changes, and error conditions to enhance integration flexibility.

- **Optimize Task Management:**  
  Refine task scheduling and reduce dynamic task creation by exploring static task allocation or task pooling to lower overhead and prevent memory fragmentation.

- **Improve Memory Management:**  
  Enhance resource utilization by moving embedded web files to PROGMEM or a filesystem like LittleFS and using static allocation for tasks and synchronization objects where feasible.

## Installation

1. **Clone the Repository:**

   ```bash
   git clone https://github.com/yourusername/AlooWifiManager.git
   ```

2. **Include in Your Project:**
   - Copy `AlooWifiManager.h` and `AlooWifiManager.cpp` into your Arduino project directory.
   - Ensure you have the ESP32 Arduino core installed along with required libraries (SPIFFS, FreeRTOS).

Or as a PlatformIO dependency:

```ini
lib_deps =
    https://github.com/rmsz005/AlooWifiManager
```

## Usage

### Basic Example

Include the library and initialize it in your `setup()` function:

```cpp
#include <Arduino.h>
#include "AlooWifiManager.h"

// Create an instance of WiFiManager with custom AP credentials.
WiFiManager wifiManager("ESP32-Config", "", "");

void setup() {
  Serial.begin(115200);
  // Start WiFi management with the web server running on a separate core.
  wifiManager.begin(true, 1, 1);
}

void loop() {
  // Process web server requests if not running on a separate core.
  wifiManager.processWebServer();
}
```

### Advanced Example

For a more complete example—including performance stats and an HTTP call upon connection—see below:

```cpp
#include <Arduino.h>
#include "AlooWifiManager.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

WiFiManager wifiManager("MyDeviceSetup", "alooaloo", "");

void performInitSessionCall() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure sslClient;
  sslClient.setInsecure(); // Note: Use a proper certificate in production

  HTTPClient https;
  https.setTimeout(8000);
  const char* url = "https://your-api-url.example.com/init-session";

  if (https.begin(sslClient, url)) {
    int httpCode = https.GET();
    if (httpCode == HTTP_CODE_OK) {
      Serial.printf("Payload: %s\n", https.getString().c_str());
    } else {
      Serial.printf("HTTP call error: %d\n", httpCode);
    }
    https.end();
  } else {
    Serial.println("Failed to connect for HTTP call.");
  }
}

void setup() {
  Serial.begin(115200);
  wifiManager.begin(true, 1, 1);
}

void loop() {
  if (wifiManager.getStatus() == WiFiStatus::CONNECTED) {
    performInitSessionCall();
    delay(10000); // Delay between HTTP calls
  }
}
```

## Contributing

Contributions are welcome! If you have suggestions, bug reports, or improvements, please open an issue or submit a pull request.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Built using the ESP32 Arduino core and FreeRTOS.
- Inspired by various WiFi management solutions available for ESP32 and ESP8266.

