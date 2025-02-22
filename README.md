

# AlooWifiManager

AlooWifiManager is an asynchronous WiFi manager library for ESP32 projects built on the Arduino framework. It simplifies WiFi connection management by providing non-blocking, event-driven connection handling with automatic fallback to AP mode and a built-in captive portal for easy configuration.

## Features

- **Asynchronous Connection Management:** Non-blocking tasks to connect and monitor WiFi status.
- **Captive Portal:** Automatically switches to AP mode when connection fails, allowing users to submit new credentials via a web interface.
- **Web Server Integration:** Built-in web server with endpoints for network scanning (`/wifinetworks`), connection status (`/status`), and credential submission (`/submit`).
- **Persistent Credential Storage:** Saves and retrieves WiFi credentials using ESP32 Preferences.
- **Customizable Parameters:** Configure connection timeouts, reconnection attempts, and task delays to fit your application’s needs.

## Installation

1. **Clone the Repository:**

   ```bash
   git clone https://github.com/yourusername/AlooWifiManager.git
   ```

2. **Include in Your Project:**
   - Copy `AlooWifiManager.h` and `AlooWifiManager.cpp` into your Arduino project directory.
   - Ensure you have the Arduino core for ESP32 installed along with required libraries (SPIFFS, FreeRTOS).

Or as platformio dependency
```
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
// (Parameters: AP SSID, AP Password, SPIFFS web directory)
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

For a more complete example, including performance stats and an HTTP call upon connection:

```cpp
#include <Arduino.h>
#include "AlooWifiManager.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

WiFiManager wifiManager("MyDeviceSetup", "alooaloo", "");

void performInitSessionCall() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure sslClient;
  sslClient.setInsecure(); // Note: Use secure method in production

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
- Inspired by various WiFi management solutions available for ESP32.

