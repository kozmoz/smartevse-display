#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <map>

#include "esp_wifi.h"
#include "esp_http_server.h"
#include <time.h>

#if defined(__cplusplus)
extern "C" {
#endif
const char *mg_unlist(size_t no);

const char *mg_unpack(const char *name, size_t *size, time_t *mtime);

struct packed_file {
    const char *name;
    const unsigned char *data;
    size_t size;
    time_t mtime;
};

extern const struct packed_file packed_files[];
#if defined(__cplusplus)
}
#endif

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

// ---- Configuration ----
Preferences preferences;
String ssid = "Juurlink123";
String password = "1122334411";
String evse_ip = "192.168.1.172";
// EVSE connected
bool evseConnected = false;
// WiFi connected
bool wifiConnected = false;

// Globale variabelen
String evseState = "Not Connected";
String mode = "Solar";
int chargeCurrent = 0;
int gridCurrent = 0;
String error = "None";

// ---- UI Elements ----
M5Canvas canvas(&M5.Lcd);
const int screenWidth = 240;
const int screenHeight = 320;

struct WifiNetwork {
    String ssid;
    int rssi;
    bool isOpen;
};

// ---- Function Prototypes ----
void showWiFiConnectingAnimation();

void stopWiFiConnectingAnimation();

void sendModeChange(const String &newMode);

void showWiFiRetryMenu();

void drawUI();

void fetchData();

void drawEVSEScreen();

void showSettingsMenu();

void configureSetting(const char *key, String &value);

bool isValidIP(const String &ip);

bool connectToWiFi(String ssid, String password);

String inputBuffer = "";
bool shiftMode = false;

const char *keyboardLayout[4][10] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
    {"a", "s", "d", "f", "g", "h", "j", "k", "l", "←"},
    {"z", "x", "c", "v", "b", "n", "m", " ", "Shift", "Enter"}
};

// Button objects
LGFX_Button solarButton;
LGFX_Button smartButton;


/**
 * Scans for available WiFi networks and retrieves their details.
 *
 * @return A vector of WifiNetwork objects, each containing information
 *         about an available WiFi network, sorted by signal strength
 *         in ascending order.
 */
static std::vector<WifiNetwork> cached_networks;
static unsigned long last_scan_time = 0;
const unsigned long SCAN_INTERVAL = 10000; // 10 seconds

std::vector<WifiNetwork> scanWifiNetworks() {
    unsigned long current_time = millis();

    if (current_time - last_scan_time < SCAN_INTERVAL) {
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
    if (data != NULL) {
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
    httpd_handle_t server = NULL;
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


void drawKeyboard() {
    M5.Lcd.fillScreen(TFT_BLACK);
    int startY = 120;

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 10; col++) {
            int x = col * 22 + 10;
            int y = row * 30 + startY;
            M5.Lcd.drawRect(x, y, 20, 28, TFT_WHITE);
            M5.Lcd.setCursor(x + 5, y + 8);
            if (shiftMode && row < 3) {
                M5.Lcd.print(keyboardLayout[row][col][0] - 32); // Toon als hoofdletter
            } else {
                M5.Lcd.print(keyboardLayout[row][col]);
            }
        }
    }

    // Weergave van de ingevoerde tekst
    M5.Lcd.setCursor(10, 80);
    M5.Lcd.fillRect(10, 80, 220, 20, TFT_BLACK); // Clear de lijn
    M5.Lcd.print(inputBuffer);
}

void handleKeyboardTouch() {
    M5.Touch.update(0);
    if (M5.Touch.getCount() > 0) {
        auto touch = M5.Touch.getDetail(0);
        int row = (touch.y - 120) / 30;
        int col = (touch.x - 10) / 22;

        if (row >= 0 && row < 4 && col >= 0 && col < 10) {
            String key = keyboardLayout[row][col];

            if (key == "←") {
                if (inputBuffer.length() > 0) {
                    inputBuffer.remove(inputBuffer.length() - 1);
                }
            } else if (key == "Shift") {
                shiftMode = !shiftMode;
            } else if (key == "Enter") {
                M5.Lcd.fillRect(10, 80, 220, 20, TFT_BLACK);
                drawUI();
                return;
            } else {
                if (shiftMode) {
                    key.toUpperCase();
                }
                inputBuffer += key;
            }
        }
    }
    drawKeyboard();
}

String showVirtualKeyboard() {
    inputBuffer = "";
    drawKeyboard();

    while (true) {
        handleKeyboardTouch();
        M5.update();
        if (M5.BtnA.wasPressed()) {
            return inputBuffer;
        }
    }
}

void displayMonochromeBitmap(WiFiClient *stream, int width, int height, int x, int y) {
    // Skip the bitmap header
    uint8_t header[67];
    stream->read(header, 67);

    // Calculate bytes per row (1 bit per pixel, 8 pixels per byte)
    const int bytesPerRow = width / 8; // Ceiling of width/8
    const int paddedBytesPerRow = bytesPerRow; // No padding, as per original code

    // Allocate the buffer for all the row data
    auto *rowData = new uint8_t[paddedBytesPerRow * height];

    // Read all pixel data into the buffer
    stream->read(rowData, paddedBytesPerRow * height);

    // Begin writing to the display with doubled dimensions
    M5.Lcd.startWrite();
    M5.Lcd.setAddrWindow(x, y, width * 2, height * 2);

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
                uint16_t pixel = (byte & (1 << bit)) ? TFT_WHITE : TFT_BLACK;
                buffer[bufferIndex++] = pixel;
                buffer[bufferIndex++] = pixel; // Double horizontally
            }
        }

        // Push the row buffer to the display twice for vertical doubling
        M5.Lcd.pushPixels(buffer, width * 2);
        M5.Lcd.pushPixels(buffer, width * 2); // Repeat row for 2x vertical scale
    }

    // Clean up
    delete[] rowData;
    M5.Lcd.endWrite();
}

void updateButtonState() {
    solarButton.setOutlineColor((mode == "Solar") ? ACTIVE_BORDER_COLOR : BACKGROUND_COLOR);
    smartButton.setOutlineColor((mode == "Smart") ? ACTIVE_BORDER_COLOR : BACKGROUND_COLOR); // White border for active
    solarButton.drawButton();
    smartButton.drawButton();
}

void playBeep() {
    M5.Speaker.tone(1000, 50); // 4000 Hz for 100 ms
}

// ---- Setup ----
void setup() {
    // Initialize M5Stack Tough
    auto cfg = M5.config();
    cfg.external_spk = true; // Enable the external speaker if available
    M5.begin(cfg);

    // M5.Display.setFont(NORMAL_FONT);
    M5.Display.setRotation(1);
    M5.Display.setTextSize(2);
    // Set display rotation
    M5.Display.fillScreen(BACKGROUND_COLOR);

    // Initialize speaker
    M5.Speaker.begin();
    M5.Speaker.setVolume(200); // Max volume for beep

    // Initialize buttons
    solarButton.initButton(&M5.Display, SOLAR_BUTTON_X + BUTTON_WIDTH / 2, BUTTON_Y + BUTTON_HEIGHT / 2,
                           BUTTON_WIDTH, BUTTON_HEIGHT, BACKGROUND_COLOR, 0xF680, TFT_BLACK, "Solar", 3);
    smartButton.initButton(&M5.Display, SMART_BUTTON_X + BUTTON_WIDTH / 2, BUTTON_Y + BUTTON_HEIGHT / 2,
                           BUTTON_WIDTH, BUTTON_HEIGHT, BACKGROUND_COLOR, 0x07E0, TFT_BLACK, "Smart", 3);


    preferences.begin("EVSE", false);
    ssid = preferences.getString("ssid", ssid);
    password = preferences.getString("password", password);
    evse_ip = preferences.getString("evse_ip", evse_ip);

    wifiConnected = connectToWiFi(ssid, password);

    if (!wifiConnected) {
        start_ap_mode();
        start_webserver();
    }
}

static unsigned long lastCheck1S = 0;
static unsigned long lastCheck2S = 0;

// ---- Main Loop ----
void loop() {
    M5.update(); // Update touch and button states

    if (wifiConnected) {
        // Check for touch events
        if (M5.Touch.getCount() > 0) {
            auto touchPoint = M5.Touch.getDetail(0);
            int x = touchPoint.x;
            int y = touchPoint.y;

            // Check Solar button
            if (solarButton.contains(x, y) && mode != "Solar") {
                mode = "Solar";
                updateButtonState();
                drawUI();
                playBeep();
                sendModeChange("2");
                solarButton.press(true);
                smartButton.press(false);
            }
            // Check Smart button
            else if (smartButton.contains(x, y) && mode != "Smart") {
                mode = "Smart";
                updateButtonState();
                drawUI();
                playBeep();
                sendModeChange("3");
                solarButton.press(false);
                smartButton.press(true);
            }
        }

        // Update every second.
        if (millis() - lastCheck1S >= 1000) {
            lastCheck1S = millis();
            drawEVSEScreen();
        }

        if (millis() - lastCheck2S >= 2000) {
            lastCheck2S = millis();
            fetchData();
            updateButtonState();
            drawUI();
        }
    }

    delay(50);
}

bool connectToWiFi(String ssid, String password) {
    //M5.Lcd.println("Connecting to WiFi...");

    WiFi.begin(ssid.c_str(), password.c_str());
    int attempts = 0;
    // showWiFiConnectingAnimation();
    while (WiFi.status() != WL_CONNECTED && attempts < 5) {
        delay(500);
        attempts++;
        // M5.Lcd.setCursor(10, 220);
        // M5.Lcd.printf("Connecting... Attempt %d", attempts);
    }
    //stopWiFiConnectingAnimation();

    if (WiFi.status() == WL_CONNECTED) {
        // M5.Lcd.setCursor(10, 220);
        // M5.Lcd.println("WiFi Connected");
        return true;
    }
    // M5.Lcd.println("Failed to connect.");
    // showWiFiRetryMenu();
    return false;
}

// ---- Fetch Data from Smart EVSE ----
void showTimeoutMessage();

void fetchData() {
    if (!wifiConnected) {
        evseConnected = false;
        return;
    }

    HTTPClient http;
    String url = "http://" + evse_ip + "/settings";
    http.begin(url);
    http.setTimeout(5000);

    int httpResponseCode = http.GET();

    if (httpResponseCode < 300) {
        String payload = http.getString();
        evseConnected = true;

        // JSON parsing
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
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
            M5.Lcd.setCursor(10, 220);
            M5.Lcd.println("JSON Parsing Failed");
        }
    } else {
        evseConnected = false;
        showTimeoutMessage();
    }

    http.end();
}


void drawUI() {
    // M5.Lcd.fillScreen(TFT_BLACK);

    // Reset error area.
    M5.Lcd.fillRect(0, 224, 340, 20, TFT_BLACK);
    // Reset mode.
    M5.Lcd.fillRect(184, 204, 340 - 184, 20, TFT_BLACK);

    M5.Lcd.setTextSize(2);

    // The WiFi Status Indicator.
    M5.Lcd.setTextColor(TFT_LIGHTGRAY);
    M5.Lcd.setCursor(16, 204);
    M5.Lcd.print("WIFI");
    M5.Lcd.fillCircle(76, 210, 5, wifiConnected ? TFT_GREEN : TFT_RED);

    // The EVSE Status Indicator.
    M5.Lcd.setTextColor(TFT_LIGHTGRAY);
    M5.Lcd.setCursor(100, 204);
    M5.Lcd.print("EVSE ");
    M5.Lcd.fillCircle(160, 210, 5, evseConnected ? TFT_GREEN : TFT_RED);

    // The Mode.
    M5.Lcd.setTextColor(TFT_LIGHTGRAY);
    M5.Lcd.setCursor(184, 204);
    M5.Lcd.print("Mode:" + mode);

    // Error
    M5.Lcd.setTextColor(error == "" || error == "None" ? TFT_DARKGRAY : TFT_RED);
    M5.Lcd.setCursor(16, 224);
    M5.Lcd.print("Error: " + error);

    // Rest text color.
    M5.Lcd.setTextColor(TEXT_COLOR);
}


// ---- Draw EVSE Screen (Live) ----
void drawEVSEScreen() {
    if (wifiConnected) {
        HTTPClient http;
        const String url = "http://" + evse_ip + "/lcd";
        http.begin(url);
        http.setTimeout(5000);

        const int httpResponseCode = http.GET();

        if (httpResponseCode == 200) {
            WiFiClient *stream = http.getStreamPtr();
            displayMonochromeBitmap(stream, 128, 64, 32, 0);
            // M5.Lcd.setCursor(10, 10);
            // M5.Lcd.setTextSize(1);
            // M5.Lcd.println("Loaded LCD screen");
        } else {
            M5.Lcd.setCursor(10, 10);
            M5.Lcd.println("Failed to load LCD screen");
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
    if (wifiConnected) {
        HTTPClient http;
        // String url = "http://" + evse_ip + "/settings?mode=" + newMode + "&starttime=0&override_current=0&repeat=0";
        String url = "http://" + evse_ip + "/settings?mode=" + newMode +
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


// ---- Timeout Message ----
void showTimeoutMessage() {
    M5.Lcd.clear(TFT_BLACK);
    M5.Lcd.setCursor(10, 40);
    M5.Lcd.println("Connection Timeout");
    M5.Lcd.println("1. Retry");
    M5.Lcd.println("2. Settings");

    while (true) {
        M5.Touch.update(0);
        if (M5.Touch.getCount() > 0) {
            auto t = M5.Touch.getDetail(0);
            if (t.y > 40 && t.y < 80) {
                fetchData();
                return;
            } else if (t.y > 80 && t.y < 120) {
                showSettingsMenu();
                return;
            }
        }
    }
}


// ---- WiFi Connecting Animation ----
void showWiFiConnectingAnimation() {
    for (int i = 0; i < 3; ++i) {
        M5.Lcd.setCursor(10, 220);
        M5.Lcd.print("Connecting to WiFi");
        for (int j = 0; j < 3; ++j) {
            M5.Lcd.print(".");
            delay(300);
        }
        M5.Lcd.fillRect(10, 220, 200, 20, TFT_BLACK);
    }
}

void stopWiFiConnectingAnimation() {
    M5.Lcd.fillRect(10, 220, 200, 20, TFT_BLACK);
}


// ---- Show WiFi Retry Menu ----
void showWiFiRetryMenu() {
    M5.Lcd.clear(TFT_BLACK);
    M5.Lcd.setCursor(10, 40);
    M5.Lcd.println("WiFi Connection Failed");
    M5.Lcd.println("1. Retry");
    M5.Lcd.println("2. Settings");

    while (true) {
        M5.Touch.update(0);
        if (M5.Touch.getCount() > 0) {
            auto t = M5.Touch.getDetail(0);
            if (t.y > 40 && t.y < 80) {
                connectToWiFi(ssid, password);
                return;
            } else if (t.y > 80 && t.y < 120) {
                showSettingsMenu();
                return;
            }
        }
    }
}


void showSettingsMenu() {
    configureSetting("SSID", ssid);
    configureSetting("Password", password);
    configureSetting("EVSE IP", evse_ip);

    // Toon het hoofdscherm opnieuw na aanpassingen
    drawUI();
}


void configureSetting(const char *key, String &value) {
    M5.Lcd.clear(TFT_BLACK);
    M5.Lcd.setCursor(10, 40);
    M5.Lcd.printf("Change %s:", key);

    String result = showVirtualKeyboard();
    if (!result.isEmpty()) {
        if (strcmp(key, "evse_ip") == 0 && !isValidIP(result)) {
            //showConfirmation("Invalid IP Address");
            return;
        }
        value = result;
        preferences.putString(key, value);
        //showConfirmation("Saved Successfully");
    }
}


// ---- IP Address Validation ----
bool isValidIP(const String &ip) {
    IPAddress addr;
    return addr.fromString(ip);
}
