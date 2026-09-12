
#include <WiFi.h>

const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const uint16_t TCP_PORT   = 8888;

#define BRIDGE_RX_PIN 16
#define BRIDGE_TX_PIN 17
#define BRIDGE_BAUD   115200

WiFiServer server(TCP_PORT);
WiFiClient client;

void setup() {
    Serial.begin(115200);
    Serial1.begin(BRIDGE_BAUD, SERIAL_8N1, BRIDGE_RX_PIN, BRIDGE_TX_PIN);

    Serial.println();
    Serial.print("Connecting to WiFi: ");
    Serial.println(WIFI_SSID);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    WiFi.setSleep(false);
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }
    Serial.println();
    Serial.print("WiFi connected. IP address: ");
    Serial.println(WiFi.localIP());

    server.begin();
    Serial.print("TCP bridge server listening on port ");
    Serial.println(TCP_PORT);
    Serial.println("Waiting for Gateway (Pi) to connect...");
}

void loop() {
    if (!client || !client.connected()) {
        WiFiClient newClient = server.available();
        if (newClient) {
            client = newClient;
            Serial.print("Gateway connected from: ");
            Serial.println(client.remoteIP());
        }
    }

    if (client && client.connected() && client.available()) {
        uint8_t buf[256];
        int n = client.read(buf, sizeof(buf));
        if (n > 0) {
            Serial1.write(buf, n);
            Serial.print("WiFi->UART: ");
            Serial.print(n);
            Serial.println(" bytes");
        }
    }

    if (Serial1.available()) {
        uint8_t buf[256];
        int n = 0;
        while (Serial1.available() && n < (int)sizeof(buf)) {
            buf[n++] = Serial1.read();
        }
        if (n > 0 && client && client.connected()) {
            client.write(buf, n);
            Serial.print("UART->WiFi: ");
            Serial.print(n);
            Serial.println(" bytes");
        }
    }
}
