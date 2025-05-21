#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <map>

#include "esp_wifi.h"
#include "esp_http_server.h"
#include <ctime>
#include <ESPmDNS.h>

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

#define WIFI_SSID "M5Stack-Config"
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

// ---- Configuration ----
Preferences preferences;
String ssid = "Juurlink";
String password = "1122334411";
String smartevse_host = "192.168.1.92";

// EVSE connected
bool evse_connected = false;
// WiFi connected
bool wifi_connected = false;
// Show config (SmartEVSE selection screen).
bool showConfig = false;

// Globale variabelen
String evseState = "Not Connected";
String mode = "Solar";
int chargeCurrent = 0;
int gridCurrent = 0;
String error = "None";

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

// ---- Function Prototypes ----
void sendModeChange(const String &newMode);

void drawUI();

void fetchSmartEVSEData();

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

esp_err_t get_handler(httpd_req_t *req) {
    if (strcmp(req->uri, "/api/wifi") == 0) {
        auto networks = scanWifiNetworks();
        JsonDocument doc;
        JsonArray array = doc.to<JsonArray>();

        for (size_t i = 0; i < networks.size(); i++) {
            JsonObject network = array.add<JsonObject>();
            network["ssid"] = networks[i].ssid;
            network["rssi"] = networks[i].rssi;
            network["open"] = networks[i].isOpen;
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
    String uri = String(req->uri);
    if (uri == "/") {
        uri = "/index.html";
    }
    auto contentType = "text/html";
    bool isCss = uri.endsWith(".css");
    bool isJs = uri.endsWith(".js");

    if (isCss) {
        contentType = "text/css";
    }
    if (isJs) {
        contentType = "application/javascript";
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

esp_err_t post_handler(httpd_req_t *req) {
    char buf[128];
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        return ESP_FAIL;
    }

    char ssid[MAX_SSID_LEN];
    char password[MAX_PASS_LEN];

    sscanf(buf, "ssid=%31[^&]&password=%63s", ssid, password);

    preferences.putString("ssid", ssid);
    preferences.putString("password", password);

    httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Location", "/success.html");
    httpd_resp_send(req, "<script>window.location='/success.html'</script>", -1);
    return ESP_OK;
}

void start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = nullptr;
    // Add wildcard support.
    // https://community.platformio.org/t/esp-http-server-h-has-no-wildcard/11732
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_start(&server, &config);

    httpd_uri_t get_uri = {
        .uri = "*",
        .method = HTTP_GET,
        .handler = get_handler
    };
    httpd_register_uri_handler(server, &get_uri);

    httpd_uri_t post_uri = {
        .uri = "/",
        .method = HTTP_POST,
        .handler = post_handler
    };
    httpd_register_uri_handler(server, &post_uri);
}

void start_ap_mode() {
    M5.begin();
    M5.Display.clear();
    M5.Display.print("Starting AP Mode...\n\n");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);

    // ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    // ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    // ESP_ERROR_CHECK(esp_wifi_start());

    auto local_ip = WiFi.softAPIP();
    M5.Display.print("AP Mode Active\nSSID: " WIFI_SSID "\nPassword: " WIFI_PASS "\nIP: " + local_ip.toString());
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

    smartEVSEConfigButton.initButton(&M5.Display, SCREEN_WIDTH/2, BUTTON_Y + BUTTON_HEIGHT / 2,
                                     SCREEN_WIDTH - (2*SOLAR_BUTTON_X), BUTTON_HEIGHT, BACKGROUND_COLOR, 0xF680, TFT_BLACK,
                                     "Select EVSE", 3);

}

void drawSmartEVSESolarSmartButton() {
    if (evse_connected) {
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
    if (!evse_connected) {
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

    preferences.begin("smartevse-display", false);
    ssid = preferences.getString("ssid", ssid);
    password = preferences.getString("password", password);
    smartevse_host = preferences.getString("smartevse_host", smartevse_host);

    wifi_connected = connectToWiFi(ssid, password);

    if (!wifi_connected) {
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

    start_webserver();

    // Reset initializing text.
    M5.Display.fillRect(16, 204, 340 - 16, 20, TFT_BLACK);

    initButtons();
}

static unsigned long lastCheck1S = 0;
static unsigned long lastCheck2S = 0;


// ---- Main Loop ----
void loop() {
    M5.update(); // Update touch and button states

    if (wifi_connected) {
        // Check for touch events
        if (M5.Touch.getCount() > 0) {
            const auto touchPoint = M5.Touch.getDetail(0);
            int16_t x = touchPoint.x;
            int16_t y = touchPoint.y;

            // Check Solar button touch.
            if (evse_connected && mode != "Solar" && solarButton.contains(x, y)) {
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
            else if (evse_connected && mode != "Smart" && smartButton.contains(x, y)) {
                mode = "Smart";
                playBeep();
                drawSmartEVSESolarSmartButton();
                drawUI();
                sendModeChange("3");
                solarButton.press(false);
                smartButton.press(true);
                smartEVSEConfigButton.press(false);
            } else if (!evse_connected && smartEVSEConfigButton.contains(x, y)) {
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
            drawSmartEVSEDisplay();
        }

        if (millis() - lastCheck2S >= 2000) {
            lastCheck2S = millis();
            fetchSmartEVSEData();
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
void fetchSmartEVSEData() {
    if (!wifi_connected) {
        evse_connected = false;
        return;
    }
    if (!smartevse_host) {
        evse_connected = false;
        error = "No SmartEVSE host";
        return;
    }

    HTTPClient http;
    String url = "http://" + smartevse_host + "/settings";
    http.begin(url);
    http.setTimeout(5000);

    int httpResponseCode = http.GET();

    if (httpResponseCode < 300) {
        String payload = http.getString();
        evse_connected = true;

        // JSON parsing
        JsonDocument doc;
        DeserializationError jsonError = deserializeJson(doc, payload);

        if (!jsonError) {
            // Extract values from JSON en update de globale variabelen
            chargeCurrent = doc["settings"]["charge_current"];
            gridCurrent = doc["phase_currents"]["TOTAL"];
            String evseStateId = doc["evse"]["state"];
            int modeId = doc["mode_id"];

            evseState = evseStateId;

            if (modeId == 2) {
                mode = "Solar";
            } else if (modeId == 3) {
                mode = "Smart";
            }
        } else {
            evse_connected = false;
            error = "SmartEVSE Failed";
        }
    } else {
        evse_connected = false;
        error = "SmartEVSE Timeout";
    }
    http.end();
}


void drawUI() {
    // M5.Display.fillScreen(TFT_BLACK);

    // Reset error area.
    M5.Display.fillRect(0, 224, 340, 20, TFT_BLACK);
    // Reset mode.
    M5.Display.fillRect(184, 204, 340 - 184, 20, TFT_BLACK);

    M5.Display.setTextSize(2);

    // The WiFi Status Indicator.
    M5.Display.setTextColor(TFT_LIGHTGRAY);
    M5.Display.setCursor(16, 204);
    M5.Display.print("WIFI");
    M5.Display.fillCircle(76, 210, 5, wifi_connected ? TFT_GREEN : TFT_RED);

    // The EVSE Status Indicator.
    M5.Display.setTextColor(TFT_LIGHTGRAY);
    M5.Display.setCursor(100, 204);
    M5.Display.print("EVSE ");
    M5.Display.fillCircle(160, 210, 5, evse_connected ? TFT_GREEN : TFT_RED);

    // The Mode.
    M5.Display.setTextColor(TFT_LIGHTGRAY);
    M5.Display.setCursor(184, 204);
    M5.Display.print("Mode:" + (evse_connected ? mode : "-"));

    // Show Error.
    M5.Display.setTextColor(error == "" || error == "None" ? TFT_DARKGRAY : TFT_RED);
    M5.Display.setCursor(16, 224);
    M5.Display.print("Error: " + error);

    // Rest text color.
    M5.Display.setTextColor(TEXT_COLOR);
}


// ---- Draw EVSE Screen (Live) ----
void drawSmartEVSEDisplay() {
    if (wifi_connected) {
        HTTPClient http;
        const String url = "http://" + smartevse_host + "/lcd";
        http.begin(url);
        http.setTimeout(5000);

        const int httpResponseCode = http.GET();

        if (httpResponseCode == 200) {
            WiFiClient *stream = http.getStreamPtr();
            displayMonochromeBitmap(stream, 128, 64, 32, 0);
        } else {
            // Display placeholder image.
            auto path = String("/data/lcd-placeholder.png").c_str();
            size_t size = 0;
            time_t mtime = 0;
            const char *data = mg_unpack(path, &size, &mtime);
            if (data != nullptr) {
                if (!M5.Display.drawPng((uint8_t *) data, size, 32, 0)) {
                    M5.Display.setTextColor(TFT_RED);
                    M5.Display.setCursor(16, 10);
                    M5.Display.println("Failed to decode PNG");
                }
            } else {
                // Thos cannot happen.
                M5.Display.setTextColor(TFT_RED);
                M5.Display.setCursor(16, 10);
                M5.Display.println("File not found");
            }
        }
        http.end();
    }
}

/**
 * Send Mode Change
 *
 * @param newMode 2 = Solar, 3 = Smart
 */
void sendModeChange(const String &newMode) {
    if (wifi_connected) {
        HTTPClient http;
        // String url = "http://" + evse_ip + "/settings?mode=" + newMode + "&starttime=0&override_current=0&repeat=0";
        String url = "http://" + smartevse_host + "/settings?mode=" + newMode +
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
    M5.Display.setTextSize(2);

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
                if (touchY >= y + i*44 && touchY < y + (i+1)*44) {
                    // Clear the screen again.
                    M5.Display.fillScreen(BACKGROUND_COLOR);
                    // Store the selected host.
                    smartevse_host = hosts[i].ip;
                    preferences.putString("smartevse_host", smartevse_host);
                    showConfig = false;
                    // Try to connect to the device.
                    fetchSmartEVSEData();
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
