#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <set>

// The CYD touch uses some non-default SPI pins
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// Define TOUCH_CS for TFT_eSPI library
#ifndef TOUCH_CS
#define TOUCH_CS XPT2046_CS
#endif

// Colors (Bluetooth theme)
#define BT_BLUE       tft.color565(0, 103, 198)
#define BT_LIGHT_BLUE tft.color565(94, 169, 255)
#define BT_DARK_BLUE  tft.color565(0, 52, 99)
#define BT_WHITE      TFT_WHITE
#define BT_BLACK      TFT_BLACK
#define BT_BACKGROUND tft.color565(10, 20, 50) // Dark blue background

// Fonts
#define TITLE_FONT  4
#define TEXT_FONT   2

// Screen dimensions
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

// Initialize SPI for touch screen
SPIClass spi_touch(VSPI);

// Initialize touch screen
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

// Initialize display
TFT_eSPI tft = TFT_eSPI();

// Bluetooth scanning variables
int scanTime = 5; // In seconds
BLEScan* pBLEScan;
int totalDevices = 0;
String allDevicesList = "";

// Touch coordinates
uint16_t touchX = 0;
uint16_t touchY = 0;

// Scanning state
bool isScanning = false;

// New variables
int usableDevices = 0;
int alertDevices = 0;
String usableDevicesList = "";
String alertDevicesList = "";
bool shieldsUp = false;
unsigned long lastAlertBlinkTime = 0;
bool alertBlinkState = false;
float triangleAngle = 0;
std::set<String> allKnownDevices;  // Stores all devices ever seen
std::set<String> sessionDevices;   // Stores devices seen in the current session

// Asynchronous scanning variables
bool scanInProgress = false;
unsigned long scanStartTime = 0;
const unsigned long scanDuration = 5000; // 5 seconds scan duration

// Function prototypes
void drawInterface();
void handleTouch();
void scanDevices();
void displayDeviceList(String devices, String title);
void drawGradientBackground();
void toggleShields();
void displayAlertList();
void drawAlertTriangles();
void drawRotatedTriangle(int centerX, int centerY, int size, float angle);
String getManufacturer(const String& macAddress);

#define HISTORY_LENGTH 15
int totalDevicesHistory[HISTORY_LENGTH] = {0};
int usableDevicesHistory[HISTORY_LENGTH] = {0};
int historyIndex = 0;
unsigned long lastHistoryUpdateTime = 0;

void updateDeviceHistory() {
    if (millis() - lastHistoryUpdateTime >= 60000) { // Update every minute
        totalDevicesHistory[historyIndex] = totalDevices;
        usableDevicesHistory[historyIndex] = usableDevices;
        historyIndex = (historyIndex + 1) % HISTORY_LENGTH;
        lastHistoryUpdateTime = millis();
    }
}

void drawGraph(int x, int y, int w, int h, int* data, int dataSize, uint16_t color) {
    int maxVal = 1; // Avoid division by zero
    for (int i = 0; i < dataSize; i++) {
        if (data[i] > maxVal) maxVal = data[i];
    }

    for (int i = 0; i < dataSize - 1; i++) {
        int x1 = x + i * w / (dataSize - 1);
        int y1 = y + h - (data[i] * h / maxVal);
        int x2 = x + (i + 1) * w / (dataSize - 1);
        int y2 = y + h - (data[(i + 1) % dataSize] * h / maxVal);
        tft.drawLine(x1, y1, x2, y2, color);
    }
}

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        String deviceAddress = advertisedDevice.getAddress().toString().c_str();
        String manufacturer = getManufacturer(deviceAddress);
        String deviceName = advertisedDevice.getName().c_str();
        String deviceInfo = deviceName.isEmpty() ? manufacturer : deviceName + " (" + manufacturer + ")";
        deviceInfo += " [";
        deviceInfo += deviceAddress;
        deviceInfo += "]";

        Serial.print("Device found: ");
        Serial.print(deviceInfo);
        Serial.print(" RSSI: ");
        Serial.println(advertisedDevice.getRSSI());

        // Always add to allKnownDevices, regardless of RSSI
        bool isNewDevice = allKnownDevices.insert(deviceAddress).second;

        // Consider devices with RSSI > -70 as usable
        if (advertisedDevice.getRSSI() > -70) {
            usableDevices++;
            usableDevicesList += deviceInfo + " RSSI: " + String(advertisedDevice.getRSSI()) + "\n";

            bool isNewSessionDevice = sessionDevices.insert(deviceAddress).second;

            if (shieldsUp && isNewDevice && isNewSessionDevice) {
                alertDevices++;
                alertDevicesList += deviceInfo + " RSSI: " + String(advertisedDevice.getRSSI()) + "\n";
                Serial.println("New alert device detected!");
            }
        }

        // Always add to total devices list
        totalDevices++;
        allDevicesList += deviceInfo + " RSSI: " + String(advertisedDevice.getRSSI()) + "\n";
    }
};

String getManufacturer(const String& macAddress) {
    String oui = macAddress.substring(0, 8);
    oui.toUpperCase();

    if (oui == "D0:03:4B") return "Apple";
    if (oui == "AC:DE:48") return "Apple";
    if (oui == "00:25:00") return "Apple";
    if (oui == "3C:E0:72") return "Apple";
    if (oui == "B8:27:EB") return "Raspberry Pi";
    if (oui == "00:1A:7D") return "Xiaomi";
    if (oui == "F8:A7:63") return "Xiaomi";
    if (oui == "00:50:F2") return "Microsoft";
    if (oui == "00:15:5D") return "Microsoft";
    if (oui == "28:11:A5") return "Google";
    if (oui == "00:1A:11") return "Google";
    if (oui == "D8:3A:DD") return "Google";
    if (oui == "00:1B:44") return "Samsung";
    if (oui == "00:15:99") return "Samsung";
    if (oui == "94:35:0A") return "Samsung";
    
    return "Unknown";
}

void setup() {
    Serial.begin(115200);
    Serial.println("BLE Monitor starting up...");

    // Initialize SPI for touch screen
    spi_touch.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);

    // Initialize touch screen
    ts.begin(spi_touch);
    ts.setRotation(1);

    // Initialize TFT display
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(BT_BLACK);

    // Initialize Bluetooth
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    // Initialize the triangle angle
    triangleAngle = 0;

    allKnownDevices.clear();
    sessionDevices.clear();

    for (int i = 0; i < HISTORY_LENGTH; i++) {
        totalDevicesHistory[i] = 0;
        usableDevicesHistory[i] = 0;
    }

    // Initial display
    drawInterface();
}

void loop() {
    handleTouch();
    updateDeviceHistory();

    static unsigned long lastScanTime = 0;
    if ((millis() - lastScanTime > 10000) && !isScanning && !scanInProgress && !shieldsUp) {
        lastScanTime = millis();
        scanDevices();
    }

    // Handle ongoing scan
    if (scanInProgress) {
        if (millis() - scanStartTime >= scanDuration) {
            pBLEScan->stop();
            scanInProgress = false;
            isScanning = false;
            Serial.println("BLE scan completed.");
            Serial.printf("Total devices: %d, Usable devices: %d, Alert devices: %d\n", 
                          totalDevices, usableDevices, alertDevices);

            drawInterface();

            if (shieldsUp && alertDevices > 0) {
                displayAlertList();
            }
            
            // Immediately start a new scan if shields are up
            if (shieldsUp) {
                scanDevices();
            }
        }
    }

    // Blink and rotate alert triangles if there are alerts
    if (alertDevices > 0) {
        if (millis() - lastAlertBlinkTime > 500) {
            lastAlertBlinkTime = millis();
            alertBlinkState = !alertBlinkState;
            triangleAngle += 15;
            if (triangleAngle >= 360) triangleAngle -= 360;
            drawInterface();
        }
    }
}

void drawGradientBackground() {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        uint16_t color = tft.color565(10, 20, 50 + (y * 150 / SCREEN_HEIGHT));
        tft.drawFastHLine(0, y, SCREEN_WIDTH, color);
    }
}

void drawInterface() {
    drawGradientBackground();

    tft.setTextColor(BT_WHITE, BT_BACKGROUND);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Bluetooth Scanner", SCREEN_WIDTH / 2, 15, TITLE_FONT);

    // Total Devices
    tft.setTextColor(BT_LIGHT_BLUE, BT_BACKGROUND);
    tft.setTextSize(2);
    tft.drawString("T " + String(totalDevices), 60, 50, TEXT_FONT);
    tft.drawRect(120, 35, 180, 30, BT_LIGHT_BLUE);
    drawGraph(122, 37, 176, 26, totalDevicesHistory, HISTORY_LENGTH, BT_LIGHT_BLUE);

    // Usable Devices
    tft.drawString("U " + String(usableDevices), 60, 80, TEXT_FONT);
    tft.drawRect(120, 65, 180, 30, BT_LIGHT_BLUE);
    drawGraph(122, 67, 176, 26, usableDevicesHistory, HISTORY_LENGTH, BT_LIGHT_BLUE);

    // Alerts
    if (alertDevices > 0) {
        tft.fillRect(0, 110, SCREEN_WIDTH, 30, TFT_RED);
        tft.setTextColor(TFT_WHITE, TFT_RED);
    } else {
        tft.setTextColor(BT_LIGHT_BLUE, BT_BACKGROUND);
    }
    tft.drawString("Alerts (" + String(alertDevices) + ")", SCREEN_WIDTH / 2, 125, TEXT_FONT);
    tft.setTextSize(1);

    int buttonWidth = 173;
    int buttonHeight = 60;
    int buttonX = (SCREEN_WIDTH - buttonWidth) / 2;
    int buttonY = SCREEN_HEIGHT - 80;

    tft.fillRoundRect(buttonX, buttonY, buttonWidth, buttonHeight, 10, shieldsUp ? TFT_RED : BT_BLUE);
    tft.fillRoundRect(buttonX + 5, buttonY + 5, buttonWidth - 10, buttonHeight - 10, 8, BT_BLACK);
    tft.fillRoundRect(buttonX + 3, buttonY + 3, buttonWidth - 6, buttonHeight - 6, 9, shieldsUp ? TFT_RED : BT_BLUE);

    tft.setTextColor(BT_WHITE, shieldsUp ? TFT_RED : BT_BLUE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.drawString(shieldsUp ? "SHIELDS UP" : "SHIELDS DOWN", SCREEN_WIDTH / 2, SCREEN_HEIGHT - 50, TEXT_FONT);
    tft.setTextSize(1);

    // Draw alert triangles
    if (alertDevices > 0) {
        drawAlertTriangles();
    }
}

void drawAlertTriangles() {
    int triangleSize = 20;
    int margin = 10;

    // Left triangle
    drawRotatedTriangle(margin + triangleSize/2, margin + triangleSize/2, triangleSize, triangleAngle);

    // Right triangle
    drawRotatedTriangle(SCREEN_WIDTH - margin - triangleSize/2, margin + triangleSize/2, triangleSize, triangleAngle);
}

void drawRotatedTriangle(int centerX, int centerY, int size, float angle) {
    float rad = angle * PI / 180.0;
    int x1 = centerX + size/2 * cos(rad);
    int y1 = centerY + size/2 * sin(rad);
    int x2 = centerX + size/2 * cos(rad + 2.09439510239);
    int y2 = centerY + size/2 * sin(rad + 2.09439510239);
    int x3 = centerX + size/2 * cos(rad + 4.18879020479);
    int y3 = centerY + size/2 * sin(rad + 4.18879020479);

    uint16_t color = alertBlinkState ? TFT_RED : TFT_WHITE;
    tft.fillTriangle(x1, y1, x2, y2, x3, y3, color);
}

void handleTouch() {
    if (ts.touched()) {
        TS_Point p = ts.getPoint();
        touchX = map(p.x, 200, 3800, 0, SCREEN_WIDTH);
        touchY = map(p.y, 200, 3800, 0, SCREEN_HEIGHT);
        delay(200);

        if (touchY > 40 && touchY < 60) {
            displayDeviceList(allDevicesList, "All Devices");
        } else if (touchY > 70 && touchY < 90) {
            displayDeviceList(usableDevicesList, "Usable Devices");
        } else if (touchY > 110 && touchY < 140) {
            displayAlertList();
        } else {
            int buttonWidth = 173;
            int buttonHeight = 60;
            int buttonX = (SCREEN_WIDTH - buttonWidth) / 2;
            int buttonY = SCREEN_HEIGHT - 80;

            if (touchX > buttonX && touchX < buttonX + buttonWidth &&
                touchY > buttonY && touchY < buttonY + buttonHeight) {
                toggleShields();
            }
        }
    }
}

void toggleShields() {
    shieldsUp = !shieldsUp;
    if (shieldsUp) {
        Serial.println("Shields UP");
        alertDevicesList = "";
        alertDevices = 0;
        sessionDevices.clear();  // Clear session devices, but keep allKnownDevices
        scanDevices(); // Start scanning immediately when shields are up
    } else {
        Serial.println("Shields DOWN");
        alertDevicesList = "";
        alertDevices = 0;
        sessionDevices.clear();
        if (scanInProgress) {
            pBLEScan->stop(); // Stop scanning if shields are turned off
            scanInProgress = false;
            isScanning = false;
            Serial.println("Scanning stopped due to shields down");
        }
    }
    drawInterface();
}

void scanDevices() {
    if (!isScanning && !scanInProgress) {
        isScanning = true;
        scanInProgress = true;
        scanStartTime = millis();
        
        Serial.println("Starting BLE scan...");

        tft.fillRect(0, SCREEN_HEIGHT - 30, SCREEN_WIDTH, 30, BT_BACKGROUND);
        tft.setTextColor(BT_LIGHT_BLUE, BT_BACKGROUND);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Scanning...", SCREEN_WIDTH / 2, SCREEN_HEIGHT - 15, TEXT_FONT);

        totalDevices = 0;
        usableDevices = 0;
        allDevicesList = "";
        usableDevicesList = "";
        // Don't clear alertDevicesList or alertDevices here

        pBLEScan->start(0, nullptr, false); // Start a continuous scan
    }
}

void displayAlertList() {
    int scrollPosition = 0;
    int maxScrollPosition = 0;
    bool redraw = true;

    // Count total lines and calculate max scroll position
    int totalLines = 0;
    for (int i = 0; i < alertDevicesList.length(); i++) {
        if (alertDevicesList[i] == '\n') totalLines++;
    }
    maxScrollPosition = max(0, totalLines - 8); // Assuming 8 lines fit on screen

    while (true) {
        if (redraw) {
            tft.fillScreen(TFT_RED);
            tft.setTextColor(TFT_WHITE, TFT_RED);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("Alert Devices", SCREEN_WIDTH / 2, 15, TITLE_FONT);

            tft.setTextDatum(TL_DATUM);
            tft.setTextWrap(false, false);

            int y = 40;
            int lineHeight = 24; // Adjust based on your font size
            int linesDisplayed = 0;
            int index = 0;

            // Find starting position for scrolling
            for (int i = 0; i < scrollPosition; i++) {
                index = alertDevicesList.indexOf('\n', index) + 1;
                if (index == 0) break;
            }

            // Display visible lines
            while (index < alertDevicesList.length() && linesDisplayed < 8) {
                int nextIndex = alertDevicesList.indexOf('\n', index);
                if (nextIndex == -1) nextIndex = alertDevicesList.length();
                String line = alertDevicesList.substring(index, nextIndex);

                // Split the line into name and details
                int detailsStart = line.indexOf(" (");
                String deviceName = line.substring(0, detailsStart);
                String deviceDetails = line.substring(detailsStart);

                tft.setTextColor(TFT_WHITE, TFT_RED);
                tft.setTextSize(2);
                tft.setCursor(13, y);
                tft.print(deviceName);

                tft.setTextColor(TFT_YELLOW, TFT_RED);
                tft.setTextSize(1);
                tft.setCursor(13, y + 16);
                tft.print(deviceDetails);

                y += lineHeight;
                index = nextIndex + 1;
                linesDisplayed++;
            }

            // Draw scroll bar
            int scrollBarHeight = SCREEN_HEIGHT - 40;
            int scrollThumbHeight = max(20, scrollBarHeight / (maxScrollPosition + 1));
            int scrollThumbY = 40 + (scrollPosition * (scrollBarHeight - scrollThumbHeight) / max(1, maxScrollPosition));
            tft.drawRect(SCREEN_WIDTH - 10, 40, 10, scrollBarHeight, TFT_WHITE);
            tft.fillRect(SCREEN_WIDTH - 8, scrollThumbY, 6, scrollThumbHeight, TFT_WHITE);

            redraw = false;
        }

        if (ts.touched()) {
            TS_Point p = ts.getPoint();
            int touchX = map(p.x, 200, 3800, 0, SCREEN_WIDTH);
            int touchY = map(p.y, 200, 3800, 0, SCREEN_HEIGHT);

            if (touchX < SCREEN_WIDTH - 20) {
                // Touch outside scroll bar, exit
                break;
            } else {
                // Touch on scroll bar, scroll
                int newScrollPosition = map(touchY, 40, SCREEN_HEIGHT - 20, 0, maxScrollPosition);
                if (newScrollPosition != scrollPosition) {
                    scrollPosition = constrain(newScrollPosition, 0, maxScrollPosition);
                    redraw = true;
                }
            }
            delay(50); // Shorter debounce time
        }

        // Add a small delay to prevent watchdog timer issues
        delay(10);
    }

    drawInterface();
}

void displayDeviceList(String devices, String title) {
    int scrollPosition = 0;
    int maxScrollPosition = 0;
    bool redraw = true;

    // Count total lines and calculate max scroll position
    int totalLines = 0;
    for (int i = 0; i < devices.length(); i++) {
        if (devices[i] == '\n') totalLines++;
    }
    maxScrollPosition = max(0, totalLines - 8); // Assuming 8 lines fit on screen

    while (true) {
        if (redraw) {
            tft.fillScreen(BT_BACKGROUND);
            tft.setTextColor(BT_BLUE, BT_BACKGROUND);
            tft.setTextDatum(MC_DATUM);
            tft.drawString(title, SCREEN_WIDTH / 2, 15, TITLE_FONT);

            tft.setTextDatum(TL_DATUM);
            tft.setTextWrap(false, false);

            int y = 40;
            int lineHeight = 24; // Adjust based on your font size
            int linesDisplayed = 0;
            int index = 0;

            // Find starting position for scrolling
            for (int i = 0; i < scrollPosition; i++) {
                index = devices.indexOf('\n', index) + 1;
                if (index == 0) break;
            }

            // Display visible lines
            while (index < devices.length() && linesDisplayed < 8) {
                int nextIndex = devices.indexOf('\n', index);
                if (nextIndex == -1) nextIndex = devices.length();
                String line = devices.substring(index, nextIndex);

                tft.setTextColor(BT_WHITE, BT_BACKGROUND);
                tft.setTextSize(1);
                tft.setCursor(13, y);
                tft.print(line);

                y += lineHeight;
                index = nextIndex + 1;
                linesDisplayed++;
            }

            // Draw scroll bar
            int scrollBarHeight = SCREEN_HEIGHT - 40;
            int scrollThumbHeight = max(20, scrollBarHeight / (maxScrollPosition + 1));
            int scrollThumbY = 40 + (scrollPosition * (scrollBarHeight - scrollThumbHeight) / max(1, maxScrollPosition));
            tft.drawRect(SCREEN_WIDTH - 10, 40, 10, scrollBarHeight, BT_LIGHT_BLUE);
            tft.fillRect(SCREEN_WIDTH - 8, scrollThumbY, 6, scrollThumbHeight, BT_LIGHT_BLUE);

            redraw = false;
        }

        if (ts.touched()) {
            TS_Point p = ts.getPoint();
            int touchX = map(p.x, 200, 3800, 0, SCREEN_WIDTH);
            int touchY = map(p.y, 200, 3800, 0, SCREEN_HEIGHT);

            if (touchX < SCREEN_WIDTH - 20) {
                // Touch outside scroll bar, exit
                break;
            } else {
                // Touch on scroll bar, scroll
                int newScrollPosition = map(touchY, 40, SCREEN_HEIGHT - 20, 0, maxScrollPosition);
                if (newScrollPosition != scrollPosition) {
                    scrollPosition = constrain(newScrollPosition, 0, maxScrollPosition);
                    redraw = true;
                }
            }
            delay(50); // Shorter debounce time
        }

        // Add a small delay to prevent watchdog timer issues
        delay(10);
    }

    drawInterface();
}