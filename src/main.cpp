#include <M5Unified.h>
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <map>

#include "esp_wifi.h"
#include "esp_http_server.h"
#include <ctime>
#include <utility>
#include <ESPmDNS.h>
#include <qrcode.h>
#include <DNSServer.h>

// The included functions are in a C file.
extern "C" {
const char *mg_unlist(size_t no);

const char *mg_unpack(const char *name, size_t *size, time_t *mtime);

struct packed_file {
    const char *name;
    const unsigned char *data;
    size_t size;
    time_t mtime;
};

extern const packed_file packed_files[];
}

#define WIFI_SSID "SmartEVSE_Display"
#define WIFI_PASS "12345678"
#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64

// Button dimensions and positions
#define BUTTON_WIDTH 128
#define BUTTON_HEIGHT 56
#define BUTTON_Y 128
#define SOLAR_BUTTON_X 16
#define SMART_BUTTON_X 176

// Colors
#define ACTIVE_BORDER_COLOR TFT_WHITE
#define TEXT_COLOR TFT_WHITE
#define BACKGROUND_COLOR TFT_BLACK

// Fonts
#define NORMAL_FONT &fonts::FreeSans12pt7b
#define BOLD_FONT &fonts::FreeSansBold12pt7b

const String DEVICE_NAME = "smartevse-display";
const String PREFERENCES_KEY_EVSE_HOST = "smartevse_host";
const String PREFERENCES_KEY_WIFI_SSID = "ssid";
const String PREFERENCES_KEY_WIFI_PASSWORD = "password";

constexpr unsigned long LONG_PRESS_TIME = 2000;

// AP_HOSTNAME will be defined in the setup().
String AP_HOSTNAME;

IPAddress apIP(192, 168, 4, 1); // Local IP of the ESP32
IPAddress subnet(255, 255, 255, 0);

Preferences preferences;
String smartEvseHost;

// EVSE connected
bool evseConnected = false;
// WiFi connected
bool wifiConnected = false;
// Show config (SmartEVSE selection screen).
bool showConfig = false;
bool dnsServerRunning = false;
bool reboot = false;

// Global variables.
String evseState = "Not Connected";
// The mode, either Solar or Smart.
String mode = "Solar";
int chargeCurrent = 0;
int gridCurrent = 0;
String error = "None";

HTTPClient *smartEvseHttpClient = nullptr;

struct WifiNetwork { // NOLINT(*-pro-type-member-init)
    String ssid;
    int rssi;
    bool isOpen;
};

struct MDNSHost {
    String host;
    String serial;
    String ip;
    int port;
};

DNSServer dnsServer;

// Button objects
LGFX_Button solarButton;
LGFX_Button smartButton;
LGFX_Button configButton;

/**
 * Scans for available WiFi networks and retrieves their details.
 *
 * @return A vector of WifiNetwork objects, each containing information
 *         about an available WiFi network, sorted by signal strength
 *         in ascending order.
 */
static std::vector<WifiNetwork> cached_networks;
static unsigned long last_scan_time = 0;
constexpr unsigned long SCAN_INTERVAL = 30000;

std::vector<WifiNetwork> scanWifiNetworks() {
    unsigned long current_time = millis();

    if (last_scan_time != 0 && current_time - last_scan_time < SCAN_INTERVAL) {
        return cached_networks;
    }
    last_scan_time = current_time;

    const int n = WiFi.scanNetworks();
    std::vector<WifiNetwork> networks;

    for (int i = 0; i < n; ++i) {
        WifiNetwork network = {
            WiFi.SSID(i),
            WiFi.RSSI(i),
            WiFi.encryptionType(i) == WIFI_AUTH_OPEN
        };
        networks.push_back(network);
    }

    // Remove duplicates, keep strongest signal
    std::map<String, WifiNetwork> unique_networks;
    for (const auto &network: networks) {
        auto it = unique_networks.find(network.ssid);
        if (it == unique_networks.end() || network.rssi > it->second.rssi) {
            unique_networks[network.ssid] = network;
        }
    }

    networks.clear();
    for (const auto &pair: unique_networks) {
        networks.push_back(pair.second);
    }

    // Sort. Best networks on top.
    std::sort(networks.begin(), networks.end(),
              [](const WifiNetwork &a, const WifiNetwork &b) {
                  return a.rssi > b.rssi;
              });

    cached_networks = networks;
    return networks;
}

static std::vector<MDNSHost> cachedMdnsHosts;
static unsigned long lastMdnsQuery = 0;
constexpr unsigned long MDNS_QUERY_INTERVAL = 30000;

std::vector<MDNSHost> discoverMDNS(const bool forceFreshList = false) {
    const unsigned long currentTime = millis();

    if (!forceFreshList && lastMdnsQuery != 0 && currentTime - lastMdnsQuery < MDNS_QUERY_INTERVAL) {
        return cachedMdnsHosts;
    }
    lastMdnsQuery = currentTime;

    std::vector<MDNSHost> hosts;
    int retryCount = 3;

    while (retryCount > 0) {
        const int n = MDNS.queryService("http", "tcp");
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                const String hostname = MDNS.hostname(i);
                // Only include SmartEVSE hosts.
                if (!hostname.startsWith("SmartEVSE-")) {
                    continue;
                }
                bool exists = false;
                for (auto &host: hosts) {
                    if (host.host == hostname) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    const String serial = hostname.substring(hostname.indexOf("-") + 1);
                    hosts.push_back({
                        hostname,
                        serial,
                        MDNS.IP(i).toString(),
                        MDNS.port(i)
                    });
                }
            }
        }
        retryCount--;
        delay(1000); // Wait between retries.
    }

    if (!hosts.empty()) {
        cachedMdnsHosts = hosts;
    }

    return !hosts.empty() ? hosts : cachedMdnsHosts;
}

esp_err_t httpGetHandler(httpd_req_t *req) {
    Serial.printf("==== Process GET request uri: %s\n", req->uri);

    if (strcmp(req->uri, "/api/wifi") == 0) {
        auto networks = scanWifiNetworks();
        JsonDocument doc;
        JsonArray array = doc.to<JsonArray>();

        for (auto &i: networks) {
            auto network = array.add<JsonObject>();
            network["ssid"] = i.ssid;
            network["rssi"] = i.rssi;
            network["open"] = i.isOpen;
        }

        String json;
        serializeJson(doc, json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, json.c_str(), static_cast<ssize_t>(json.length()));
        return ESP_OK;
    }

    if (strcmp(req->uri, "/api/mdns") == 0) {
        auto hosts = discoverMDNS();
        JsonDocument doc;
        JsonArray array = doc.to<JsonArray>();

        for (auto &host: hosts) {
            auto network = array.add<JsonObject>();
            network["host"] = host.host;
        }

        String json;
        serializeJson(doc, json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, json.c_str(), static_cast<ssize_t>(json.length()));
        return ESP_OK;
    }

    size_t size = 0;
    time_t mtime = 0;
    auto uri = String(req->uri);
    if (uri == "/") {
        uri = "/index.html";
    }
    auto contentType = "text/html";
    bool isCss = uri.endsWith(".css");
    bool isJs = uri.endsWith(".js");

    if (isCss) {
        contentType = "text/css";
    } else if (isJs) {
        contentType = "application/javascript";
    }

    // Do we need to reboot the device?
    if (uri.indexOf("?reboot=true") > -1) {
        reboot = true;
    }

    // Strip everything from the URL from "?"
    if (uri.indexOf("?") > -1) {
        uri = uri.substring(0, uri.indexOf("?"));
    }

    auto path = String("/data" + uri).c_str();
    auto data = mg_unpack(path, &size, &mtime);
    if (data != nullptr) {
        httpd_resp_set_type(req, contentType);
        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&mtime));
        httpd_resp_set_hdr(req, "Last-Modified", timeStr);
        httpd_resp_send(req, data, static_cast<ssize_t>(size));
        return ESP_OK;
    }

    String notFound = "Not found 404";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_send(req, notFound.c_str(), static_cast<ssize_t>(notFound.length()));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpPostHandler(httpd_req_t *req) {
    Serial.printf("==== Process POST request uri: %s\n", req->uri);

    char buf[128];
    const int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        Serial.printf("==== Error receiving response\n");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Error", -1);
        return ESP_FAIL;
    }
    const auto payload = String(buf, ret);

    JsonDocument doc;
    const DeserializationError jsonError = deserializeJson(doc, payload);

    if (!jsonError) {
        // Extract values from JSON and update global variables.
        const String ssid = doc["ssid"];
        const String password = !doc["password"].isNull() ? doc["password"] : String("");

        if (ssid == nullptr || ssid.isEmpty()) {
            Serial.printf("==== httpPostHandler(): ssid is empty\n");
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "Error", -1);
            return ESP_FAIL;
        }

        preferences.putString(PREFERENCES_KEY_WIFI_SSID.c_str(), ssid);
        preferences.putString(PREFERENCES_KEY_WIFI_PASSWORD.c_str(), password);

        Serial.printf("==== httpPostHandler(): Save ssid to preferences: %s\n", ssid.c_str());
        Serial.printf("==== httpPostHandler(): Save password to preferences\n");

        httpd_resp_set_status(req, "201 Created");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "OK", -1);
        return ESP_OK;
    }
    Serial.printf("==== httpPostHandler(): Error parsing JSON: %s\n", jsonError.c_str());
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Error", -1);
    return ESP_FAIL;
}

void startWebserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = nullptr;
    // Add wildcard support.
    // https://community.platformio.org/t/esp-http-server-h-has-no-wildcard/11732
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_start(&server, &config);

    httpd_uri_t get_uri = {
        .uri = "*",
        .method = HTTP_GET,
        .handler = httpGetHandler
    };
    httpd_register_uri_handler(server, &get_uri);

    httpd_uri_t post_uri = {
        .uri = "/",
        .method = HTTP_POST,
        .handler = httpPostHandler
    };
    httpd_register_uri_handler(server, &post_uri);
}

void drawQRCode(const char *url, const int scale = 4, const int y = -1, const int x = -1) {
    QRCode qrcode;

    // QR code buffer, version 3 = 29x29 matrix.
    uint8_t qrcodeData[qrcode_getBufferSize(3)];

    // Initialize the QR code
    qrcode_initText(&qrcode, qrcodeData, 3, ECC_LOW, url);
    const int qrSize = qrcode.size;
    const int scaledSize = qrSize * scale;

    // If x and y are not specified (-1), center the QR code.
    const int xOffset = (x == -1) ? (M5.Display.width() - scaledSize) / 2 : x;
    const int yOffset = (y == -1) ? (M5.Display.height() - scaledSize) / 2 : y;

    // Draw the QR code
    for (int row = 0; row < qrSize; row++) {
        for (int col = 0; col < qrSize; col++) {
            int color = qrcode_getModule(&qrcode, col, row) ? BLACK : WHITE;
            M5.Display.fillRect(xOffset + col * scale, yOffset + row * scale, scale, scale, color);
        }
    }
}

String generateWiFiUrl(const char *ssid, const char *password, const bool hidden = false) {
    String url = "WIFI:";
    url += "T:WPA;";
    url += "S:" + String(ssid) + ";";
    url += "P:" + String(password) + ";";
    if (hidden) {
        url += "H:true;";
    }
    url += ";";
    return url;
}

void startApMode() {
    M5.Display.clearDisplay();
    M5.Display.setCursor(0, 0);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.print("Starting AP Mode...\n\n");

    WiFiClass::mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, subnet);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);

    // DNS redirect: capture all domains to the ESP32's IP
    const bool started = dnsServer.start(53, "*", apIP);
    Serial.printf("==== DNS Server start: %s\n", started ? "success" : "failed");
    dnsServerRunning = true;

    int y = 0;
    M5.Display.clearDisplay();
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(0, y);
    M5.Display.print("Access Point Active");
    y += 30;
    M5.Display.setCursor(0, y);
    M5.Display.print("1. Connect to WiFi");
    y += 20;
    M5.Display.setCursor(0, y);
    M5.Display.print("   SSID: " WIFI_SSID "\n"
        "   Pass: " WIFI_PASS "\n");
    y += 36;
    M5.Display.setCursor(0, y);
    M5.Display.print("2. Open in browser");
    y += 20;
    M5.Display.setCursor(0, y);
    M5.Display.print("   " + apIP.toString());

    const String url = generateWiFiUrl(WIFI_SSID, WIFI_PASS);
    const int qrX = M5.Display.width() - 120;
    const int qrY = M5.Display.height() - 120;
    // auto url2 = ("http://" + local_ip.toString()).c_str;
    drawQRCode(url.c_str(), 4, qrY, qrX);
}

void displayMonochromeBitmap(WiFiClient *stream, const int width, const int height, const int x, const int y,
                             const int foregroundColor = TFT_WHITE, const int backgroundColor = TFT_BLACK) {
    // Skip the bitmap header.
    // The BMP header is off, should be 62, figure out what's going on.
    // Uint8_t header[62];
    uint8_t header[67];
    stream->read(header, sizeof(header));

    // Calculate bytes per row (1 bit per pixel, 8 pixels per byte)
    const int bytesPerRow = width / 8; // Ceiling of width/8
    const int paddedBytesPerRow = bytesPerRow; // No padding, as per original code

    // Allocate the buffer for all the row data
    auto *rowData = new uint8_t[paddedBytesPerRow * height];

    // Read all pixel data into the buffer
    stream->read(rowData, paddedBytesPerRow * height);

    // Begin writing to the display with doubled dimensions
    M5.Display.startWrite();
    M5.Display.setAddrWindow(x, y, width * 2, height * 2);

    uint16_t buffer[256]; // Buffer for one doubled row (max 128 * 2 = 256 pixels)

    // Function to reverse bit order (for mirroring fix)
    auto reverseBits = [](uint8_t b) -> uint8_t {
        b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
        b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
        b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
        return b;
    };

    // Process rows from bottom to top
    for (int row = height - 1; row >= 0; --row) {
        int bufferIndex = 0;

        // Get the current row's data
        const uint8_t *currentRow = rowData + row * paddedBytesPerRow;

        // Process each byte in the row
        for (int col = 0; col < bytesPerRow; ++col) {
            uint8_t byte = reverseBits(currentRow[col]); // Reverse bits to fix mirroring

            // Process each bit in the byte (left to right)
            for (int bit = 0; bit < 8 && col * 8 + bit < width; ++bit) {
                // Duplicate each pixel horizontally (2 pixels per original pixel)
                uint16_t pixel = (byte & (1 << bit)) ? foregroundColor : backgroundColor;
                buffer[bufferIndex++] = pixel;
                buffer[bufferIndex++] = pixel; // Double horizontally
            }
        }

        // Push the row buffer to the display twice for vertical doubling
        M5.Display.pushPixels(buffer, width * 2);
        M5.Display.pushPixels(buffer, width * 2); // Repeat row for 2x vertical scale
    }

    // Clean up
    delete[] rowData;
    M5.Display.endWrite();
}

/**
 * Initialize all button.
 */
void initButtons() {
    solarButton.initButton(&M5.Display, SOLAR_BUTTON_X + BUTTON_WIDTH / 2, BUTTON_Y + BUTTON_HEIGHT / 2,
                           BUTTON_WIDTH, BUTTON_HEIGHT, BACKGROUND_COLOR, 0xF680, TFT_BLACK, "Solar", 3);
    smartButton.initButton(&M5.Display, SMART_BUTTON_X + BUTTON_WIDTH / 2, BUTTON_Y + BUTTON_HEIGHT / 2,
                           BUTTON_WIDTH, BUTTON_HEIGHT, BACKGROUND_COLOR, 0x07E0, TFT_BLACK, "Smart", 3);

    configButton.initButton(&M5.Display, static_cast<int16_t>(M5.Display.width() / 2), BUTTON_Y + BUTTON_HEIGHT / 2,
                            M5.Display.width() - (2 * SOLAR_BUTTON_X), BUTTON_HEIGHT, BACKGROUND_COLOR, 0xF680,
                            TFT_BLACK,
                            "Select EVSE", 3);
}

void drawSolarButton(const bool pressed = false) {
    solarButton.setFillColor(0xF680);
    solarButton.setOutlineColor(mode == "Solar" ? ACTIVE_BORDER_COLOR : BACKGROUND_COLOR);
    solarButton.drawButton(pressed);
}

void drawSmartButton(const bool pressed = false) {
    smartButton.setFillColor(0x07E0);
    smartButton.setOutlineColor(mode == "Smart" ? ACTIVE_BORDER_COLOR : BACKGROUND_COLOR);
    smartButton.drawButton(pressed);
}

void drawConfigButton(const bool pressed = false) {
    configButton.setFillColor(TFT_RED);
    configButton.setOutlineColor(ACTIVE_BORDER_COLOR);
    configButton.drawButton(pressed);
}

/**
 * Clear the complete button area.
 */
void clearButtonsArea() {
    M5.Display.fillRect(0, BUTTON_Y, M5.Display.width(), BUTTON_HEIGHT, BACKGROUND_COLOR);
}

/**
 * Plays a beep sound using the speaker with specified frequency and duration.
 *
 * @param frequency The frequency of the tone in Hz. Defaults to 1000 Hz.
 * @param duration The duration of the tone in milliseconds. Defaults to 50 ms.
 */
void playBeep(const float frequency = 1000, const int duration = 50) {
    M5.Speaker.tone(frequency, duration);
}

static unsigned long pressStartTime = 0;
static bool isLongPress = false;

void handleTouchInput(const bool touchDetected) {
    if (!touchDetected) {
        isLongPress = false;
        pressStartTime = 0;
        solarButton.press(false);
        smartButton.press(false);
        configButton.press(false);
        return;
    }

    const auto touchPoint = M5.Touch.getDetail(0);
    const int16_t x = touchPoint.x;
    const int16_t y = touchPoint.y;

    // Check for long press in the top area of the display.
    if (y < 128) {
        if (pressStartTime == 0) {
            pressStartTime = millis();
        } else if (millis() - pressStartTime >= LONG_PRESS_TIME && !isLongPress) {
            Serial.printf("==== handleTouchInput() long press detected\n");
            isLongPress = true;
            configButton.press(true);
        }
    }

    if (evseConnected) {
        bool solarButtonPressed = solarButton.contains(x, y);
        bool smartButtonPressed = smartButton.contains(x, y);
        // SmartEVSE connected.
        solarButton.press(solarButtonPressed);
        smartButton.press(smartButtonPressed);
        configButton.press(false);
        if (solarButtonPressed) {
            Serial.printf("==== handleTouchInput() solarButtonPressed\n");
        } else if (smartButtonPressed) {
            Serial.printf("==== handleTouchInput() smartButtonPressed\n");
        }
    } else {
        // No SmartEVSE connected.
        solarButton.press(false);
        smartButton.press(false);
        bool configButtonPressed = configButton.contains(x, y);
        configButton.press(configButtonPressed);

        if (configButtonPressed) {
            Serial.printf("==== handleTouchInput() configButtonPressed\n");
        }
    }
}

bool connectToWiFi(const String &ssid, const String &password) {
    Serial.printf("==== connectToWiFi() ssid: %s\n", ssid.c_str());

    WiFi.begin(ssid.c_str(), password.c_str());
    int attempts = 0;

    while (WiFiClass::status() != WL_CONNECTED && attempts < 10) {
        delay(500);
        attempts++;
    }
    return WiFiClass::status() == WL_CONNECTED;
}

// ---- Fetch Data from Smart EVSE ----
void showTimeoutMessage();

/**
 * Fetches settings and status data from the SmartEVSE server.
 *
 * This method communicates with the SmartEVSE device through an HTTP
 * request to retrieve current settings and operational data. If the
 * device is unreachable or the network is not connected, it updates
 * the state to indicate disconnection.
 */
void fetchSmartEVSEData() {
    const String ERROR_NO_HOST = "No SmartEVSE host";
    const String ERROR_JSON_FAILED = "SmartEVSE Failed";
    const String ERROR_TIMEOUT = "SmartEVSE Timeout";

    Serial.printf("==== fetchSmartEVSEData() for host: \"%s\"\n", smartEvseHost.c_str());
    if (!wifiConnected) {
        evseConnected = false;
        return;
    }
    if (smartEvseHost == nullptr || smartEvseHost.isEmpty()) {
        Serial.printf("==== fetchSmartEVSEData() smartEvseHost is empty\n");
        evseConnected = false;
        error = ERROR_NO_HOST;
        return;
    }

    HTTPClient http;
    const String url = "http://" + smartEvseHost + ".local/settings";
    http.begin(url);
    http.setTimeout(1500);

    const int httpResponseCode = http.GET();
    Serial.printf("==== fetchSmartEVSEData() httpResponseCode: %d\n", httpResponseCode);

    if (httpResponseCode >= 200 && httpResponseCode < 300) {
        String payload = http.getString();
        evseConnected = true;

        // JSON parsing.
        JsonDocument doc;
        const DeserializationError jsonError = deserializeJson(doc, payload);

        if (!jsonError) {
            // Extract values from JSON and update the global variables.
            chargeCurrent = doc["settings"]["charge_current"];
            gridCurrent = doc["phase_currents"]["TOTAL"];
            const String evseStateText = doc["evse"]["state"];
            const int modeId = doc["mode_id"];

            evseState = evseStateText;
            switch (modeId) {
                case 0:
                    mode = "Off";
                    break;
                case 1:
                    mode = "Normal";
                    break;
                case 2:
                    mode = "Solar";
                    break;
                case 3:
                    mode = "Smart";
                    break;
                case 4:
                    mode = "Pause";
                    break;
                default:
                    mode = "Unknown";
                    break;
            }

            // Clear any SmartEVSE-related error.
            if (error == ERROR_NO_HOST || error == ERROR_JSON_FAILED || error == ERROR_TIMEOUT) {
                error = "";
            }
        } else {
            evseConnected = false;
            error = ERROR_JSON_FAILED;
            Serial.printf("==== fetchSmartEVSEData() parsing JSON failed\n");
        }
    } else {
        evseConnected = false;
        error = ERROR_TIMEOUT;
    }
    http.end();
}

void drawStatus() {
    // Reset status and text area.
    M5.Display.fillRect(0, 204, M5.Display.width(), 20, TFT_BLACK);
    // Reset error area.
    M5.Display.fillRect(0, 224, M5.Display.width(), 20, TFT_BLACK);

    M5.Display.setTextSize(2);

    // The WiFi Status Indicator.
    M5.Display.setTextColor(TFT_LIGHTGRAY);
    M5.Display.setCursor(16, 204);
    M5.Display.print("WIFI");
    M5.Display.fillCircle(76, 210, 5, wifiConnected ? TFT_GREEN : TFT_RED);

    // The EVSE Status Indicator.
    M5.Display.setTextColor(TFT_LIGHTGRAY);
    M5.Display.setCursor(100, 204);
    M5.Display.print("EVSE ");
    M5.Display.fillCircle(160, 210, 5, evseConnected ? TFT_GREEN : TFT_RED);

    // The Mode.
    M5.Display.setTextColor(TFT_LIGHTGRAY);
    M5.Display.setCursor(184, 204);
    M5.Display.print("Mode:" + (evseConnected ? mode : "-"));

    // Show Error.
    M5.Display.setTextColor(error == "" || error == "None" ? TFT_DARKGRAY : TFT_RED);
    M5.Display.setCursor(16, 224);
    M5.Display.print("Error: " + error);

    // Rest text color.
    M5.Display.setTextColor(TEXT_COLOR);
}


void drawSmartEvseNoConnection() {
    constexpr int imageX = 32;
    // Display placeholder image.
    size_t size = 0;
    time_t mtime = 0;
    const auto path = String("/data/lcd-placeholder.png").c_str();
    const char *data = mg_unpack(path, &size, &mtime);

    if (data == nullptr) {
        // This cannot happen, show error.
        M5.Display.setTextColor(TFT_RED);
        M5.Display.setCursor(imageX, 10);
        M5.Display.println("File not found");
        return;
    }

    // Display the "No Conn" image.
    if (!M5.Display.LGFXBase::drawPng(reinterpret_cast<const uint8_t *>(data), size, imageX, 0)) {
        M5.Display.setTextColor(TFT_RED);
        M5.Display.setCursor(imageX, 10);
        M5.Display.println("Failed to decode PNG");
    }
}

/**
 * Draw the SmartEVSE LCD screen.
 * If not connected to a network, do nothing.
 */
void drawSmartEvseDisplay() {
    if (!wifiConnected) {
        return;
    }

    if (smartEvseHost == nullptr || smartEvseHost.isEmpty()) {
        drawSmartEvseNoConnection();
        return;
    }

    if (smartEvseHttpClient == nullptr) {
        const String url = "http://" + smartEvseHost + ".local/lcd";
        smartEvseHttpClient = new HTTPClient();
        smartEvseHttpClient->begin(url);
        smartEvseHttpClient->setTimeout(750);
        smartEvseHttpClient->addHeader("User-Agent", "SmartEVSE-display");
        smartEvseHttpClient->addHeader("Connection", "keep-alive");
        smartEvseHttpClient->addHeader("Accept", "image/bmp");
    }

    const int httpResponseCode = smartEvseHttpClient->GET();
    Serial.printf("==== drawSmartEVSEDisplay() httpResponseCode: %d\n", httpResponseCode);

    if (httpResponseCode >= 200 && httpResponseCode < 300) {
        // The call was successful.
        WiFiClient *stream = smartEvseHttpClient->getStreamPtr();
        displayMonochromeBitmap(stream, 128, 64, 32, 0);
        smartEvseHttpClient->end();
        return;
    }

    // Force it to create a new http client the next time.
    smartEvseHttpClient->end();
    delete smartEvseHttpClient;
    smartEvseHttpClient = nullptr;

    // No connection.
    drawSmartEvseNoConnection();
}

/**
 * Send Mode Change.
 *
 * @param newMode 2 = Solar, 3 = Smart
 */
void sendModeChange(const String &newMode) {
    const String ERROR_MODE_FAILED = "Mode failed";

    if (!wifiConnected) {
        return;
    }

    HTTPClient http;
    // String url = "http://" + evse_ip + "/settings?mode=" + newMode + "&starttime=0&override_current=0&repeat=0";
    String url = "http://" + smartEvseHost + ".local/settings?mode=" + newMode +
                 "&override_current=0&starttime=2025-05-15T00:27&stoptime=2025-05-15T00:27&repeat=0";

    http.begin(url);
    // http.addHeader("Content-Type", "application/json");
    http.addHeader("Content-Length", "0");

    // String jsonPayload = (newMode == "2") ?
    //     "{\"starttime\":0,\"mode\":\"2\"}" :
    //     "{\"starttime\":0,\"mode\":\"3\",\"override_current\":0}";
    String jsonPayload = "";
    const int httpResponseCode = http.POST(jsonPayload);

    if (httpResponseCode >= 200 && httpResponseCode < 300) {
        const String payload = http.getString();
        // JSON parsing
        JsonDocument doc;
        const DeserializationError jsonError = deserializeJson(doc, payload);
        if (!jsonError) {
            // Clear all errors related to mode.
            if (error == ERROR_MODE_FAILED) {
                error = "";
            }

            String modeId = doc["mode"];
            if (modeId == "2") {
                mode = "Solar";
            } else if (modeId == "3") {
                mode = "Smart";
            } else {
                Serial.printf("==== sendModeChange() failed, received unexpected modeId: %s\n", modeId.c_str());
                error = ERROR_MODE_FAILED;
            }
        } else {
            Serial.printf("=== sendModeChange() failed, JSON deserialization failed: %s\n", jsonError.c_str());
            error = ERROR_MODE_FAILED;
        }
    } else {
        Serial.printf("==== sendModeChange() failed, httpResponseCode: %d\n", httpResponseCode);
        error = ERROR_MODE_FAILED;
    }
    http.end();
}

/**
 * Scan the network and show the list of SmartEVSE devices found.
 * The user is able to select the device.
 */
void drawSmartEvseDeviceSelection() {
    // Clear screen
    M5.Display.fillScreen(BACKGROUND_COLOR);
    M5.Display.setTextColor(TEXT_COLOR);
    M5.Display.setTextSize(2);

    // Show the loading message.
    M5.Display.setCursor(0, 0);
    M5.Display.print("Discovering");
    M5.Display.setCursor(0, 20);
    M5.Display.print("SmartEVSE devices.");
    M5.Display.setCursor(0, 60);
    M5.Display.print("Please wait...");

    // Get the list of SmartEVSE devices.
    const auto hosts = discoverMDNS(true);

    // Clear the screen again.
    M5.Display.fillScreen(BACKGROUND_COLOR);

    if (hosts.empty()) {
        M5.Display.setCursor(16, 16);
        M5.Display.print("No SmartEVSE devices \n");
        M5.Display.print("found.");
        delay(5000);
        return;
    }

    // Draw header
    M5.Display.setCursor(16, 16);
    M5.Display.print("Select device:");

    // Draw the device list, max 4 devices.
    // Create a vector of buttons for host selection.
    std::vector<LGFX_Button> deviceButtons;
    std::vector<String> buttonLabels;

    // During the resize process, the default constructor is called for each button,
    // which creates default (uninitialized) objects.
    const unsigned newSize = std::min(hosts.size(), static_cast<size_t>(4));
    deviceButtons.resize(newSize);

    // Initialize and draw buttons
    int y = 48;
    for (size_t i = 0; i < deviceButtons.size(); i++) {
        auto &button = deviceButtons[i];
        auto longLabel = ("SN" + hosts[i].serial + " " + hosts[i].ip);
        buttonLabels.push_back(longLabel);

        Serial.printf("==== drawSettingsMenu() label: %s\n", longLabel.c_str());
        button.initButton(&M5.Display,
                          static_cast<int16_t>(M5.Display.width() / 2), // x center
                          static_cast<int16_t>(y + 18), // y center (36 px height button)
                          static_cast<int16_t>(M5.Display.width() - 32), // width
                          36, // height
                          TFT_DARKGREY, // fill
                          TFT_WHITE, // outline
                          TFT_BLACK, // text
                          longLabel.c_str(), // label
                          2 // text size
        );
        // We use the "long_name" feature by providing the label as a parameter to drawButton.
        button.drawButton(false, longLabel.c_str());
        y += 44;
    }

    // Process any pending touch events and wait for release.
    M5.update();
    while (M5.Touch.getCount() > 0) {
        M5.update();
    }

    smartEvseHost = "";
    while (smartEvseHost == "") {
        M5.update();
        if (M5.Touch.getCount() > 0) {
            const auto touchPoint = M5.Touch.getDetail(0);
            const int16_t touchX = touchPoint.x;
            const int16_t touchY = touchPoint.y;

            for (auto &button: deviceButtons) {
                button.press(button.contains(touchX, touchY));
            }
        } else {
            for (auto &button: deviceButtons) {
                button.press(false);
            }
        }

        for (size_t i = 0; i < deviceButtons.size(); i++) {
            const String &label = buttonLabels[i];
            if (deviceButtons[i].justPressed()) {
                playBeep(1000);
                deviceButtons[i].drawButton(true, label.c_str());
            } else if (deviceButtons[i].justReleased()) {
                deviceButtons[i].drawButton(false, label.c_str());
                smartEvseHost = hosts[i].host;
                preferences.putString(PREFERENCES_KEY_EVSE_HOST.c_str(), smartEvseHost);
                break;
            }
        }
    }
    // Clear the screen and return to the loop.
    M5.Display.fillScreen(BACKGROUND_COLOR);
}

#ifndef UNIT_TEST

// ---- Setup ----
void setup() {
    Serial.begin(115200);

    // Determine the hostname, it's based on the serial number.
    AP_HOSTNAME = DEVICE_NAME + "-" + String(static_cast<uint32_t>(ESP.getEfuseMac()) & 0xffff, 10);

    // Initialize M5Stack Tough
    auto cfg = m5::M5Unified::config();
    cfg.external_spk = true; // Enable the external speaker if available
    M5.begin(cfg);

    // M5.Display.setFont(NORMAL_FONT);
    M5.Display.setRotation(1);
    M5.Display.setTextSize(2);
    M5.Display.setColor(TFT_WHITE);
    // Set display rotation
    M5.Display.fillScreen(BACKGROUND_COLOR);

    // Display is ready, start initializing.
    M5.Display.setCursor(16, 204);
    M5.Display.print("Initializing...");
    M5.Display.display();

    // Initialize speaker
    M5.Speaker.begin();
    M5.Speaker.setVolume(200); // Max volume for beep

    preferences.begin("se-display", false);
    // Returns an empty String by default if the key doesn't exist.
    const String ssid = preferences.getString(PREFERENCES_KEY_WIFI_SSID.c_str());
    const String password = preferences.getString(PREFERENCES_KEY_WIFI_PASSWORD.c_str());
    smartEvseHost = preferences.getString(PREFERENCES_KEY_EVSE_HOST.c_str());

    Serial.printf("==== ssid from preferences: %s\n", ssid != nullptr ? ssid.c_str() : "NULL");
    Serial.printf("==== password from preferences: %s\n", password != nullptr ? password.c_str() : "NULL");
    Serial.printf("==== smartevse_host from preferences: %s\n",
                  smartEvseHost != nullptr ? smartEvseHost.c_str() : "NULL");

    // Connect to WiFi; try three times max.
    if (!ssid.isEmpty()) {
        int retries = 3;
        while (retries > 0) {
            wifiConnected = connectToWiFi(ssid, password != nullptr ? password : "");
            if (wifiConnected) {
                break;
            }
            retries--;
            if (retries > 0) {
                delay(1000);
            }
        }
    }

    if (!wifiConnected) {
        startApMode();
    }

    // Initialize MDNS.
    int retries = 5;
    while (!MDNS.begin(AP_HOSTNAME.c_str()) && retries-- > 0) {
        delay(1000);
    }

    if (retries <= 0) {
        error = "Error starting mDNS";
    } else {
        MDNS.addService("http", "tcp", 80); // announce Web server
    }

    startWebserver();

    initButtons();

    if (wifiConnected) {
        fetchSmartEVSEData();
        if (evseConnected) {
            drawSolarButton(false);
            drawSmartButton(false);
        } else {
            drawConfigButton(false);
        }
    }
}


static unsigned long lastCheck1S = 0;
static unsigned long lastCheck3S = 0;

// ---- Main Loop ----
void loop() {
    // Update touch and button states.
    M5.update();

    // Reboot device?
    if (reboot) {
        Serial.printf("==== Rebooting...\n");
        // Some delay to finish possible http response.
        delay(2000);
        esp_restart();
    }

    // Must be called frequently.
    if (dnsServerRunning) {
        dnsServer.processNextRequest();
    }

    if (wifiConnected) {
        // Check for touch events for the three buttons.
        const bool touchDetected = M5.Touch.getCount() > 0;
        handleTouchInput(touchDetected);

        // Draw and update buttons.
        if (solarButton.justPressed()) {
            Serial.printf("==== Loop - solarButton.justPressed()\n");
            playBeep(1000);
            mode = "Solar";
            drawSolarButton(true);
        }
        if (smartButton.justPressed()) {
            Serial.printf("==== Loop - smartButton.justPressed()\n");
            playBeep(2000);
            mode = "Smart";
            drawSmartButton(true);
        }
        const bool solarButtonReleased = solarButton.justReleased();
        const bool smartButtonReleased = smartButton.justReleased();
        if (solarButtonReleased || smartButtonReleased) {
            Serial.printf("==== Loop - solar- or smartButton.justReleased()\n");
            // Update the active state of both buttons.
            drawSolarButton(false);
            drawSmartButton(false);

            sendModeChange(solarButtonReleased ? "2" : "3");
        }
        if (configButton.justPressed()) {
            Serial.printf("==== Loop - configButton.justPressed()\n");
            playBeep(1000);
            drawConfigButton(true);
        }
        if (configButton.justReleased()) {
            Serial.printf("==== Loop - configButton.justReleased()\n");
            drawConfigButton(false);
            drawSmartEvseDeviceSelection();

            // Clear errors and buttons.
            error = "";
            clearButtonsArea();
            evseConnected = false;
            drawStatus();
        }

        // Update every second.
        if (millis() - lastCheck1S >= 1000) {
            lastCheck1S = millis();
            Serial.printf("==== Loop 1s - drawSmartEVSEDisplay...\n");
            drawSmartEvseDisplay();
        }

        if (millis() - lastCheck3S >= 3000) {
            lastCheck3S = millis();
            Serial.printf("==== Loop 3s - Fetching data...\n");

            // Improvement? Move fetchSmartEVSEData() to FreeRTOS task?
            const bool previousEvseConnected = evseConnected;
            const String previousMode = mode;
            fetchSmartEVSEData();
            drawStatus();

            // If the status of the SmartEVSE is changed, update the buttons accordingly.
            if (evseConnected != previousEvseConnected) {
                clearButtonsArea();
                if (evseConnected) {
                    drawSolarButton(false);
                    drawSmartButton(false);
                } else {
                    drawConfigButton(false);
                }
            }

            // If the mode changed, update the outline of the border,
            if (mode != previousMode) {
                drawSolarButton();
                drawSmartButton();
            }
        }
    }
}
#endif // UNIT_TEST
