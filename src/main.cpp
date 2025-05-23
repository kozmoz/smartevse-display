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

extern const struct packed_file packed_files[];
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
const String AP_HOSTNAME = DEVICE_NAME + "-" + String((uint32_t) ESP.getEfuseMac() & 0xffff, 10);

IPAddress apIP(192, 168, 4, 1); // Local IP of the ESP32
IPAddress subnet(255, 255, 255, 0);

// ---- Configuration ----
Preferences preferences;
String smartevseHost;

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

// ---- UI Elements ----
M5Canvas canvas(&M5.Display);
const int SCREEN_WIDTH = 320;
const int SCREEN_HEIGHT = 240;

struct WifiNetwork {
    String ssid;
    int rssi;
    bool isOpen;
};

struct MDNSHost {
    String host;
    String ip;
    int port;
};

DNSServer dnsServer;

// ---- Function Prototypes ----
void sendModeChange(const String &newMode);

void drawUI();

void fetchSmartEVSEData(void *param);

void drawSmartEVSEDisplay();

void drawSettingsMenu();

bool isValidIP(const String &ip);

bool connectToWiFi(String ssid, String password);

// Button objects
LGFX_Button solarButton;
LGFX_Button smartButton;
LGFX_Button smartEVSEConfigButton;

/**
 * Scans for available WiFi networks and retrieves their details.
 *
 * @return A vector of WifiNetwork objects, each containing information
 *         about an available WiFi network, sorted by signal strength
 *         in ascending order.
 */
static std::vector<WifiNetwork> cached_networks;
static unsigned long last_scan_time = 0;
const unsigned long SCAN_INTERVAL = 30000; // 10 seconds

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

static std::vector<MDNSHost> cached_mdns_hosts;
static unsigned long last_mdns_query = 0;
const unsigned long MDNS_QUERY_INTERVAL = 30000; // 10 seconds

std::vector<MDNSHost> discoverMDNS() {
    unsigned long current_time = millis();

    if (last_mdns_query != 0 && current_time - last_mdns_query < MDNS_QUERY_INTERVAL) {
        return cached_mdns_hosts;
    }
    last_mdns_query = current_time;

    std::vector<MDNSHost> hosts;
    int retry_count = 3;

    while (retry_count > 0) {
        const int n = MDNS.queryService("http", "tcp");
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                const String hostname = MDNS.hostname(i);
                if (!hostname.startsWith("SmartEVSE")) {
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
                    hosts.push_back({
                        hostname,
                        MDNS.IP(i).toString(),
                        MDNS.port(i)
                    });
                }
            }
        }
        retry_count--;
        delay(1000); // Wait between retries.
    }

    if (!hosts.empty()) {
        cached_mdns_hosts = hosts;
    }

    return !hosts.empty() ? hosts : cached_mdns_hosts;
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

        for (size_t i = 0; i < hosts.size(); i++) {
            JsonObject network = array.add<JsonObject>();
            network["host"] = hosts[i].host;
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
            Serial.printf("==== ssid is empty\n");
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "Error", -1);
            return ESP_FAIL;
        }

        preferences.putString("ssid", ssid);
        preferences.putString("password", password);

        Serial.printf("==== ssid to preferences: %s\n", ssid.c_str());
        Serial.printf("==== password to preferences: %s\n", password.c_str());

        httpd_resp_set_status(req, "201 Created");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "OK", -1);
        return ESP_OK;
    }
    Serial.printf("==== Error parsing JSON: %s\n", jsonError.c_str());
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
    url += "T:WPA;"; // Encryption type (Update to "T:nopass;" if open network).
    url += "S:" + String(ssid) + ";";
    url += "P:" + String(password) + ";";
    if (hidden) {
        url += "H:true;";
    }
    url += ";";
    return url;
}

void start_ap_mode() {
    M5.Display.clearDisplay();
    M5.Display.setCursor(0, 0);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.print("Starting AP Mode...\n\n");

    WiFi.mode(WIFI_AP);
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

void displayMonochromeBitmap(WiFiClient *stream, int width, int height, int x, int y,
                             int fcolor = TFT_WHITE, int bcolor = TFT_BLACK) {
    // Skip the bitmap header
    // Todo: The header is off.
    // uint8_t header[62];
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
                uint16_t pixel = (byte & (1 << bit)) ? fcolor : bcolor;
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

    smartEVSEConfigButton.initButton(&M5.Display, SCREEN_WIDTH / 2, BUTTON_Y + BUTTON_HEIGHT / 2,
                                     SCREEN_WIDTH - (2 * SOLAR_BUTTON_X), BUTTON_HEIGHT, BACKGROUND_COLOR, 0xF680,
                                     TFT_BLACK,
                                     "Select EVSE", 3);
}

void drawSmartEVSESolarSmartButton() {
    if (evseConnected) {
        // Clear buttons.
        M5.Display.fillRect(0, BUTTON_Y, 420, BUTTON_HEIGHT, BACKGROUND_COLOR);

        solarButton.setFillColor(0xF680);
        smartButton.setFillColor(0x07E0);
        // White border for active
        solarButton.setOutlineColor((mode == "Solar") ? ACTIVE_BORDER_COLOR : BACKGROUND_COLOR);
        smartButton.setOutlineColor((mode == "Smart") ? ACTIVE_BORDER_COLOR : BACKGROUND_COLOR);
        solarButton.drawButton();
        smartButton.drawButton();
    }
}

void drawSmartEVSEConfigButton() {
    if (!evseConnected) {
        // Clear buttons.
        M5.Display.fillRect(0, BUTTON_Y, 420, BUTTON_HEIGHT, BACKGROUND_COLOR);

        smartEVSEConfigButton.setFillColor(TFT_RED);
        smartEVSEConfigButton.setOutlineColor(ACTIVE_BORDER_COLOR);
        smartEVSEConfigButton.drawButton();
    }
}

void playBeep() {
    M5.Speaker.tone(1000, 50); // 4000 Hz for 100 ms
}

#ifndef UNIT_TEST

// ---- Setup ----
void setup() {
    Serial.begin(115200);

    // Initialize M5Stack Tough
    auto cfg = M5.config();
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
    const String ssid = preferences.getString("ssid");
    const String password = preferences.getString("password");
    // smartevse_host = preferences.getString("smartevse_host");
    smartevseHost = "192.168.1.92";

    Serial.printf("==== ssid from preferences: %s\n", ssid != nullptr ? ssid.c_str() : "NULL");
    Serial.printf("==== password from preferences: %s\n", password != nullptr ? password.c_str() : "NULL");
    Serial.printf("==== smartevse_host from preferences: %s\n",
                  smartevseHost != nullptr ? smartevseHost.c_str() : "NULL");

    if (ssid.length() > 0) {
        wifiConnected = connectToWiFi(ssid, password != nullptr ? password : "");
    }

    if (!wifiConnected) {
        start_ap_mode();
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
}

static unsigned long lastCheck1S = 0;
static unsigned long lastCheck2S = 0;


// Todo: Move drawSmartEVSEDisplay() to FreeRTOS task

// ---- Main Loop ----
void loop() {
    // Update touch and button states
    M5.update();

    // Reboot device?
    if (reboot) {
        Serial.printf("==== Rebooting...\n");
        delay(2000);
        esp_restart();
        reboot = false;
    }

    // Must be called frequently.
    if (dnsServerRunning) {
        dnsServer.processNextRequest();
    }

    if (wifiConnected) {
        // Check for touch events
        if (M5.Touch.getCount() > 0) {
            const auto touchPoint = M5.Touch.getDetail(0);
            const int16_t x = touchPoint.x;
            const int16_t y = touchPoint.y;

            // Check Solar button touch.
            if (evseConnected && mode != "Solar" && solarButton.contains(x, y)) {
                mode = "Solar";
                playBeep();
                drawSmartEVSESolarSmartButton();
                drawUI();
                sendModeChange("2");
                solarButton.press(true);
                smartButton.press(false);
                smartEVSEConfigButton.press(false);
            }
            // Check Smart button touch.
            else if (evseConnected && mode != "Smart" && smartButton.contains(x, y)) {
                mode = "Smart";
                playBeep();
                drawSmartEVSESolarSmartButton();
                drawUI();
                sendModeChange("3");
                solarButton.press(false);
                smartButton.press(true);
                smartEVSEConfigButton.press(false);
            } else if (!evseConnected && smartEVSEConfigButton.contains(x, y)) {
                showConfig = true;
                playBeep();
                solarButton.press(false);
                smartButton.press(false);
                smartEVSEConfigButton.press(true);
                drawSettingsMenu();
            }
        }

        // Update every second.
        if (millis() - lastCheck1S >= 1000) {
            lastCheck1S = millis();
            Serial.printf("==== Loop 1s - drawSmartEVSEDisplay...\n");
            drawSmartEVSEDisplay();
        }

        if (false && millis() - lastCheck2S >= 3000) {
            lastCheck2S = millis();
            Serial.printf("==== Loop 3s - Fetching data...\n");
            fetchSmartEVSEData(nullptr);
            // xTaskCreatePinnedToCore(fetchSmartEVSEData, "NetTask", 8192, NULL, 1, NULL, 1);
            drawSmartEVSESolarSmartButton();
            drawSmartEVSEConfigButton();
            drawUI();
        }
    }

    delay(50);
}
#endif // UNIT_TEST

bool connectToWiFi(String ssid, String password) {
    //M5.Display.println("Connecting to WiFi...");

    WiFi.begin(ssid.c_str(), password.c_str());
    int attempts = 0;
    // showWiFiConnectingAnimation();
    while (WiFi.status() != WL_CONNECTED && attempts < 5) {
        delay(500);
        attempts++;
        // M5.Display.setCursor(10, 220);
        // M5.Display.printf("Connecting... Attempt %d", attempts);
    }
    //stopWiFiConnectingAnimation();

    if (WiFi.status() == WL_CONNECTED) {
        // M5.Display.setCursor(10, 220);
        // M5.Display.println("WiFi Connected");
        return true;
    }
    // M5.Display.println("Failed to connect.");
    // showWiFiRetryMenu();
    return false;
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
void fetchSmartEVSEData(void *param) {
    if (!wifiConnected) {
        evseConnected = false;
        return;
    }
    if (!smartevseHost) {
        evseConnected = false;
        error = "No SmartEVSE host";
        return;
    }

    HTTPClient http;
    String url = "http://" + smartevseHost + "/settings";
    http.begin(url);
    http.setTimeout(1500);

    const int httpResponseCode = http.GET();

    if (httpResponseCode < 300) {
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
            if (modeId == 2) {
                mode = "Solar";
            } else if (modeId == 3) {
                mode = "Smart";
            }
            // Todo: Handle other modes.
        } else {
            evseConnected = false;
            error = "SmartEVSE Failed";
        }
    } else {
        evseConnected = false;
        error = "SmartEVSE Timeout";
    }
    http.end();
    vTaskDelete(nullptr); // End this task
}


void drawUI() {
    // M5.Display.fillScreen(TFT_BLACK);

    // Reset status and text area.
    M5.Display.fillRect(0, 204, M5.Display.width(), 20, TFT_BLACK);
    // Reset error area.
    M5.Display.fillRect(0, 224, M5.Display.width(), 20, TFT_BLACK);
    // Reset mode.
    // M5.Display.fillRect(184, 204, 340 - 184, 20, TFT_BLACK);

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


// ---- Draw EVSE Screen (Live) ----
void drawSmartEVSEDisplay() {
    if (!wifiConnected) {
        return;
    }

    if (smartEvseHttpClient == nullptr) {
        const String url = "http://" + smartevseHost + "/lcd";
        smartEvseHttpClient = new HTTPClient();
        smartEvseHttpClient->begin(url);
        smartEvseHttpClient->setTimeout(750);
        smartEvseHttpClient->addHeader("User-Agent", "SmartEVSE-display");
        smartEvseHttpClient->addHeader("Connection", "keep-alive");
        smartEvseHttpClient->addHeader("Accept", "image/bmp");
    }

    const int httpResponseCode = smartEvseHttpClient->GET();
    Serial.printf("==== drawSmartEVSEDisplay() httpResponseCode: %d\n", httpResponseCode);

    constexpr int imageX = 32;
    if (httpResponseCode < 300) {
        // The call was successful.
        WiFiClient *stream = smartEvseHttpClient->getStreamPtr();
        displayMonochromeBitmap(stream, 128, 64, imageX, 0);
        smartEvseHttpClient->end();
        return;
    }

    // Force it to create a new http client the next time.
    smartEvseHttpClient->end();
    delete smartEvseHttpClient;
    smartEvseHttpClient = nullptr;

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
    if (!M5.Display.drawPng(reinterpret_cast<const uint8_t *>(data), size, imageX, 0)) {
        M5.Display.setTextColor(TFT_RED);
        M5.Display.setCursor(imageX, 10);
        M5.Display.println("Failed to decode PNG");
    }
}

/**
 * Send Mode Change
 *
 * @param newMode 2 = Solar, 3 = Smart
 */
void sendModeChange(const String &newMode) {
    if (wifiConnected) {
        HTTPClient http;
        // String url = "http://" + evse_ip + "/settings?mode=" + newMode + "&starttime=0&override_current=0&repeat=0";
        String url = "http://" + smartevseHost + "/settings?mode=" + newMode +
                     "&override_current=0&starttime=2025-05-15T00:27&stoptime=2025-05-15T00:27&repeat=0";

        http.begin(url);
        // http.addHeader("Content-Type", "application/json");
        http.addHeader("Content-Length", "0");

        // String jsonPayload = (newMode == "2") ?
        //     "{\"starttime\":0,\"mode\":\"2\"}" :
        //     "{\"starttime\":0,\"mode\":\"3\",\"override_current\":0}";
        String jsonPayload = "";

        int httpResponseCode = http.POST(jsonPayload);

        if (httpResponseCode < 300) {
            String payload = http.getString();
            // JSON parsing
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);

            if (!error) {
                String modeId = doc["mode"];
                if (modeId == "2") {
                    mode = "Solar";
                } else if (modeId == "3") {
                    mode = "Smart";
                }
            }
        } else {
            error = "Mode failed";
        }
        http.end();
    }
}


void drawSettingsMenu() {
    // Clear screen
    M5.Display.fillScreen(BACKGROUND_COLOR);
    M5.Display.setTextColor(TEXT_COLOR);
    M5.Display.setTextSize(3);

    // Show the loading message.
    M5.Display.setCursor(0, 0);
    M5.Display.print("Discovering \nSmartEVSE devices.\n\nPlease wait...");

    // Get the list of SmartEVSE devices.
    const auto hosts = discoverMDNS();

    // Clear the screen again.
    M5.Display.fillScreen(BACKGROUND_COLOR);

    if (hosts.empty()) {
        M5.Display.setCursor(16, 16);
        M5.Display.print("No SmartEVSE devices \nfound.");
        // Todo show http config screen URL
        return;
    }

    // Draw header
    M5.Display.setCursor(16, 16);
    M5.Display.print("Select device:");

    // Draw the device list, max 4 devices.
    int y = 48;
    for (size_t i = 0; i < hosts.size() && i < 4; i++) {
        M5.Display.fillRect(16, y, SCREEN_WIDTH - 32, 36, TFT_DARKGREY);
        M5.Display.setCursor(24, y + 8);
        M5.Display.print(hosts[i].host);
        y += 44;
    }

    // Handle the touch selection.
    while (true) {
        M5.update();
        if (M5.Touch.getCount() > 0) {
            const auto touchPoint = M5.Touch.getDetail(0);
            const int16_t touchY = touchPoint.y;

            for (size_t i = 0; i < hosts.size() && i < 4; i++) {
                if (touchY >= y + i * 44 && touchY < y + (i + 1) * 44) {
                    playBeep();
                    // Clear the screen again.
                    M5.Display.fillScreen(BACKGROUND_COLOR);
                    // Store the selected host.
                    smartevseHost = hosts[i].ip;
                    preferences.putString("smartevse_host", smartevseHost);
                    showConfig = false;
                    // Try to connect to the device.
                    fetchSmartEVSEData(nullptr);
                    return;
                }
            }
        }
        delay(50);
    }
}


// ---- IP Address Validation ----
bool isValidIP(const String &ip) {
    IPAddress addr;
    return addr.fromString(ip);
}
