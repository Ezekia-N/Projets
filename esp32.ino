#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ======================================================
// CONFIGURATION ESP32
// ======================================================

// Identifiant UNIQUE de cet ESP32
const char* ESP32_ID = "ESP32_A";

// Utilisateur associé
// L'application pourra modifier cet identifiant plus tard.
String USER_ID = "";

// ======================================================
// WIFI - RESEAU RASPBERRY PI
// ======================================================

const char* GATEWAY_SSID = "Connecteo-Gateway";
const char* GATEWAY_PASSWORD = "CHANGE_ME_GATEWAY";

// ======================================================
// WIFI - POINT D'ACCES TELEPHONE
// ======================================================

const char* AP_PASSWORD = "Connecteo123";

// Chaque ESP32 possède son propre réseau.
// Le téléphone trouvera :
//
// Connecteo-ESP32_A
//
// IP du téléphone -> DHCP
// IP ESP32        -> 192.168.4.1
// ======================================================

IPAddress apIP(192, 168, 4, 1);
IPAddress apGateway(192, 168, 4, 1);
IPAddress apSubnet(255, 255, 255, 0);

// ======================================================
// RASPBERRY PI
// ======================================================

// IP fixe du Raspberry sur son réseau Connecteo
const char* GATEWAY_IP = "192.168.50.1";

const int GATEWAY_PORT = 8080;

// ======================================================
// SERVEUR HTTP ESP32
// ======================================================

WebServer server(80);

// ======================================================
// QUEUE DE MESSAGES
// ======================================================

#define MAX_MESSAGES 20

String messageQueue[MAX_MESSAGES];

int queueCount = 0;

// ======================================================
// UTILITAIRES
// ======================================================

void printMessage(
    const String& prefix,
    const String& message
) {
    Serial.println();
    Serial.println("==============================");
    Serial.println(prefix);
    Serial.println(message);
    Serial.println("==============================");
}

// ======================================================
// AJOUT MESSAGE QUEUE
// ======================================================

void enqueueMessage(
    const String& message
) {

    if (queueCount >= MAX_MESSAGES) {

        Serial.println(
            "QUEUE PLEINE"
        );

        return;
    }

    messageQueue[queueCount] = message;

    queueCount++;

    Serial.print(
        "Message ajouté. Queue = "
    );

    Serial.println(queueCount);
}

// ======================================================
// RECUPERATION MESSAGE TELEPHONE
// ======================================================

void handleInbox() {

    JsonDocument doc;

    JsonArray messages =
        doc["messages"].to<JsonArray>();

    for (
        int i = 0;
        i < queueCount;
        i++
    ) {

        JsonDocument message;

        deserializeJson(
            message,
            messageQueue[i]
        );

        messages.add(message);
    }

    String output;

    serializeJson(
        doc,
        output
    );

    server.send(
        200,
        "application/json",
        output
    );
}

// ======================================================
// DELETE / ACK
// ======================================================

void handleAck() {

    String messageId =
        server.arg("messageId");

    if (messageId.length() == 0) {

        server.send(
            400,
            "application/json",
            "{\"error\":\"messageId missing\"}"
        );

        return;
    }

    for (
        int i = 0;
        i < queueCount;
        i++
    ) {

        JsonDocument doc;

        deserializeJson(
            doc,
            messageQueue[i]
        );

        String id =
            doc["messageId"] | "";

        if (id == messageId) {

            for (
                int j = i;
                j < queueCount - 1;
                j++
            ) {

                messageQueue[j] =
                    messageQueue[j + 1];
            }

            queueCount--;

            Serial.print(
                "Message ACK: "
            );

            Serial.println(
                messageId
            );

            break;
        }
    }

    server.send(
        200,
        "application/json",
        "{\"ok\":true}"
    );
}

// ======================================================
// MESSAGE DU TELEPHONE
// ======================================================

void handleMessage() {

    if (
        !server.hasArg("plain")
    ) {

        server.send(
            400,
            "application/json",
            "{\"error\":\"body missing\"}"
        );

        return;
    }

    String body =
        server.arg("plain");

    printMessage(
        "MESSAGE DU TELEPHONE",
        body
    );

    JsonDocument doc;

    DeserializationError error =
        deserializeJson(
            doc,
            body
        );

    if (error) {

        server.send(
            400,
            "application/json",
            "{\"error\":\"invalid json\"}"
        );

        return;
    }

    // Ajouter l'ESP32 comme source
    doc["source"] = ESP32_ID;

    String message;

    serializeJson(
        doc,
        message
    );

    // Envoyer vers Raspberry
    sendToGateway(
        message
    );

    server.send(
        200,
        "application/json",
        "{\"ok\":true,\"route\":\"gateway\"}"
    );
}

// ======================================================
// PRESENCE
// ======================================================

void handlePresence() {

    JsonDocument doc;

    doc["esp32Id"] = ESP32_ID;
    doc["userId"] = USER_ID;
    doc["phoneConnected"] = true;
    doc["timestamp"] = millis();

    String body;

    serializeJson(
        doc,
        body
    );

    sendToGateway(
        body,
        "/presence"
    );

    server.send(
        200,
        "application/json",
        "{\"ok\":true}"
    );
}

// ======================================================
// CONFIGURATION USER
// ======================================================

void handleSetUser() {

    if (
        !server.hasArg("plain")
    ) {

        server.send(
            400,
            "application/json",
            "{\"error\":\"body missing\"}"
        );

        return;
    }

    JsonDocument doc;

    deserializeJson(
        doc,
        server.arg("plain")
    );

    USER_ID =
        doc["userId"] | "";

    Serial.print(
        "USER_ID = "
    );

    Serial.println(
        USER_ID
    );

    server.send(
        200,
        "application/json",
        "{\"ok\":true}"
    );
}

// ======================================================
// STATUS
// ======================================================

void handleStatus() {

    JsonDocument doc;

    doc["esp32Id"] = ESP32_ID;
    doc["userId"] = USER_ID;
    doc["apIp"] =
        WiFi.softAPIP().toString();

    doc["gatewayConnected"] =
        WiFi.status() == WL_CONNECTED;

    doc["gatewayIp"] =
        WiFi.localIP().toString();

    doc["phoneCount"] =
        WiFi.softAPgetStationNum();

    doc["queue"] =
        queueCount;

    String output;

    serializeJson(
        doc,
        output
    );

    server.send(
        200,
        "application/json",
        output
    );
}

// ======================================================
// ENVOI AU RASPBERRY
// ======================================================

void sendToGateway(
    const String& body,
    const char* endpoint = "/message"
) {

    if (
        WiFi.status() != WL_CONNECTED
    ) {

        Serial.println(
            "Raspberry non connecté"
        );

        return;
    }

    HTTPClient http;

    String url =
        "http://" +
        String(GATEWAY_IP) +
        ":" +
        String(GATEWAY_PORT) +
        endpoint;

    Serial.print(
        "POST Gateway : "
    );

    Serial.println(
        url
    );

    http.begin(url);

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    int response =
        http.POST(body);

    Serial.print(
        "Gateway HTTP = "
    );

    Serial.println(
        response
    );

    if (
        response > 0
    ) {

        Serial.println(
            http.getString()
        );
    }

    http.end();
}

// ======================================================
// CONFIGURATION WIFI
// ======================================================

void setupWiFi() {

    WiFi.mode(
        WIFI_AP_STA
    );

    // ============================================
    // AP POUR TELEPHONE
    // ============================================

    WiFi.softAPConfig(
        apIP,
        apGateway,
        apSubnet
    );

    String ssid =
        String("Connecteo-") +
        ESP32_ID;

    WiFi.softAP(
        ssid.c_str(),
        AP_PASSWORD,
        6,
        false,
        4
    );

    Serial.println();
    Serial.println(
        "AP TELEPHONE"
    );

    Serial.print(
        "SSID : "
    );

    Serial.println(
        ssid
    );

    Serial.print(
        "IP : "
    );

    Serial.println(
        WiFi.softAPIP()
    );

    // ============================================
    // STA -> RASPBERRY
    // ============================================

    WiFi.begin(
        GATEWAY_SSID,
        GATEWAY_PASSWORD
    );

    Serial.println(
        "Connexion au Raspberry..."
    );

    unsigned long start =
        millis();

    while (
        WiFi.status() != WL_CONNECTED &&
        millis() - start < 20000
    ) {

        delay(500);

        Serial.print(".");
    }

    Serial.println();

    if (
        WiFi.status() ==
        WL_CONNECTED
    ) {

        Serial.println(
            "Raspberry connecté"
        );

        Serial.print(
            "IP STA : "
        );

        Serial.println(
            WiFi.localIP()
        );

    } else {

        Serial.println(
            "Raspberry indisponible"
        );
    }
}

// ======================================================
// SETUP HTTP
// ======================================================

void setupServer() {

    server.on(
        "/status",
        HTTP_GET,
        handleStatus
    );

    server.on(
        "/inbox",
        HTTP_GET,
        handleInbox
    );

    server.on(
        "/ack",
        HTTP_POST,
        handleAck
    );

    server.on(
        "/message",
        HTTP_POST,
        handleMessage
    );

    server.on(
        "/presence",
        HTTP_POST,
        handlePresence
    );

    server.on(
        "/set-user",
        HTTP_POST,
        handleSetUser
    );

    server.begin();

    Serial.println(
        "HTTP ESP32 démarré"
    );
}

// ======================================================
// SETUP
// ======================================================

void setup() {

    Serial.begin(
        115200
    );

    delay(1000);

    Serial.println();
    Serial.println(
        "================================"
    );
    Serial.println(
        "      CONNECTEO ESP32"
    );
    Serial.println(
        "================================"
    );

    Serial.print(
        "ESP32 ID : "
    );

    Serial.println(
        ESP32_ID
    );

    setupWiFi();

    setupServer();
}

// ======================================================
// LOOP
// ======================================================

unsigned long lastPresence =
    0;

void loop() {

    server.handleClient();

    // Présence toutes les 10 secondes
    if (
        millis() - lastPresence > 10000
    ) {

        lastPresence =
            millis();

        if (
            USER_ID.length() > 0
        ) {

            JsonDocument doc;

            doc["esp32Id"] =
                ESP32_ID;

            doc["userId"] =
                USER_ID;

            doc["phoneConnected"] =
                WiFi.softAPgetStationNum() > 0;

            doc["timestamp"] =
                millis();

            String body;

            serializeJson(
                doc,
                body
            );

            sendToGateway(
                body,
                "/presence"
            );
        }
    }

    delay(2);
}
