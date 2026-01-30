#include <ESP32QRCodeReader.h>
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// --- CONFIG & GLOBALS ---

QueueHandle_t urlQueue;

struct UrlMessage {
  int amount; // In cents (100 = ₱1.00)
};

// ---------------------- PAYMONGO LOGIC ----------------------

String createPaymentIntent(int amountCents) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    http.begin(client, "https://api.paymongo.com/v1/payment_intents");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", BASIC_AUTH_HEADER); 

    // Prepare JSON payload
    JsonDocument doc;
    JsonObject data = doc["data"].to<JsonObject>();
    JsonObject attr = data["attributes"].to<JsonObject>();
    attr["amount"] = amountCents;
    attr["payment_method_allowed"][0] = "qrph";
    attr["currency"] = "PHP";
    attr["capture_type"] = "automatic";

    String requestBody;
    String intentId;
    serializeJson(doc, requestBody);

    int httpCode = http.POST(requestBody);
    if (httpCode == 200) {
        String response = http.getString();
        JsonDocument respDoc;
        deserializeJson(respDoc, response);
        intentId = respDoc["data"]["id"].as<String>();
        
        Serial.println("Intent Created: " + intentId);
    } else {
        Serial.printf("Intent failed, error: %d\n", httpCode);
        Serial.println(http.getString());
    }
    http.end();
    return intentId;
}

String createPaymentMethod() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    http.begin(client, "https://api.paymongo.com/v1/payment_methods");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", BASIC_AUTH_HEADER);

    // Create the JSON payload from your cURL
    JsonDocument doc;
    JsonObject attr = doc["data"]["attributes"].to<JsonObject>();
    attr["type"] = "qrph";
    
    JsonObject billing = attr["billing"].to<JsonObject>();
    billing["name"] = "Storage Customer";
    billing["email"] = "customer@example.com";
    billing["phone"] = "09171234567";
    
    JsonObject addr = billing["address"].to<JsonObject>();
    addr["line1"] = "123 Quezon Ave";
    addr["city"] = "Quezon City";
    addr["country"] = "PH";

    String requestBody;
    serializeJson(doc, requestBody);

    int httpCode = http.POST(requestBody);
    String pmId = "";

    if (httpCode == 200 || httpCode == 201) {
        String response = http.getString();
        JsonDocument respDoc;
        deserializeJson(respDoc, response);
        pmId = respDoc["data"]["id"].as<String>();
        Serial.println("Payment Method Created: " + pmId);
    } else {
        Serial.printf("PM Creation failed: %d\n", httpCode);
        Serial.println(http.getString());
    }

    http.end();
    return pmId;
}

String attachPaymentMethod(String intentId, String pmId) {
    WiFiClientSecure client;
    client.setInsecure(); 
    HTTPClient http;

    String url = "https://api.paymongo.com/v1/payment_intents/" + intentId + "/attach";
    
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", BASIC_AUTH_HEADER); // Use your Secret Key (sk_live)

    // Build the JSON body using the pmId argument
    JsonDocument doc;
    doc["data"]["attributes"]["payment_method"] = pmId;

    String requestBody;
    serializeJson(doc, requestBody);
    
    int httpCode = http.POST(requestBody);
    String qrData = "";

    if (httpCode == 200 || httpCode == 201) {
        String response = http.getString();
        JsonDocument respDoc;
        DeserializationError error = deserializeJson(respDoc, response);
        
        if (!error) {
            // PayMongo returns the QR image URL inside the next_action object
            qrData = respDoc["data"]["attributes"]["next_action"]["code"]["image_url"].as<String>();
            Serial.println("Success! QR Code URL received.");
        } else {
            Serial.print("JSON Parse Error: ");
            Serial.println(error.c_str());
        }
    } else {
        Serial.printf("Attach failed, error: %d\n", httpCode);
        Serial.println(http.getString());
    }

    http.end();
    return qrData; // This is the URL of the QR Ph image
}

bool pollPaymentStatus(String intentId) {
    unsigned long startPoll = millis();
    unsigned long timeout = 180000; // 3 minutes timeout
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    while (millis() - startPoll < timeout) {
        
        unsigned long elapsed = millis() - startPoll;
        unsigned long remaining = (timeout - elapsed) / 1000;
        Serial.printf("Checking payment status... [%lus remaining]\n", remaining);

        String url = "https://api.paymongo.com/v1/payment_intents/" + intentId;
        
        http.begin(client, url);
        http.addHeader("Accept", "application/json");
        http.addHeader("Authorization", BASIC_AUTH_HEADER);

        int httpCode = http.GET();
        if (httpCode == 200) {
            String response = http.getString();
            JsonDocument doc;
            deserializeJson(doc, response);
            
            String status = doc["data"]["attributes"]["status"].as<String>();
            Serial.println("Payment Status: " + status);

            if (status == "succeeded") {
                Serial.println("💰 PAYMENT SUCCESSFUL!");
                http.end();
                return true;
            } else if (status == "cancelled") {
                Serial.println("❌ Payment Cancelled.");
                http.end();
                return false;
            }
        } else {
            Serial.printf("Polling failed (%d)\n", httpCode);
        }
        
        http.end();
        delay(3000); // Wait 3 seconds before checking again to avoid rate limits
    }
    
    Serial.println("⏰ Polling timed out.");
    return false;
}

// ---------------------- TASKS ----------------------

void httpTask(void *pvParameters) {
  UrlMessage msg;
  while (true) {
    if (xQueueReceive(urlQueue, &msg, portMAX_DELAY) == pdPASS) {
      Serial.printf("Starting PayMongo flow for %d cents...\n", msg.amount);
      
      String intentId = createPaymentIntent(msg.amount);
      String pmId = createPaymentMethod();
      
      if (intentId != "" && pmId != "") {
          String qrData = attachPaymentMethod(intentId, pmId);
          
          if (qrData != "") {
              Serial.println("Scan this QR Code URL to pay: ");
              Serial.println(qrData);
              
              // --- START POLLING ---
              if (pollPaymentStatus(intentId)) {
                  Serial.println("ACTION: Unlocking Storage Box...");
                  // digitalWrite(LOCK_PIN, HIGH); 
              } else {
                  Serial.println("ACTION: Payment failed or timed out.");
              }
          }
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  // WiFi Setup (using your existing logic)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  urlQueue = xQueueCreate(1, sizeof(UrlMessage));
  xTaskCreatePinnedToCore(httpTask, "HTTP_Task", 12 * 1024, NULL, 4, NULL, 1);

  // Trigger a test payment of ₱1.00
  UrlMessage testMsg = {100};
  xQueueSend(urlQueue, &testMsg, 0);
}

void loop() {}