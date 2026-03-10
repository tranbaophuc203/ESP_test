#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <DHT.h>

// ================= CẤU HÌNH =================
const char* ssid = "Yen Vy";
const char* password = "12345679";

// Cấu hình HiveMQ Cloud
const char* mqtt_server = "704fe78be9c049179073f7cf8ac31e12.s1.eu.hivemq.cloud"; // Thay bằng Cluster URL của bạn
const int mqtt_port = 8883; 
const char* mqtt_user = "esp32_user";             // Thay bằng Username
const char* mqtt_pass = "User.1234";              // Thay bằng Password

// Cấu hình DHT11
#define DHTPIN 4       // Chân nối DHT11
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// Cấu hình Giao thức UART
#define BLOCK_SIZE 256
#define START_BYTE 0xAA
#define ACK 0x79
#define NACK 0x1F

// Khởi tạo đối tượng
WiFiClientSecure espClient;
PubSubClient client(espClient);
unsigned long lastMsg = 0;

// --- BỔ SUNG BIẾN TRẠNG THÁI ---
bool is_stm32_in_boot = false;
unsigned long last_heartbeat = 0;
#define BOOT_HEARTBEAT 0xBC

// ================= HÀM HỖ TRỢ =================
void setup_wifi() {
    delay(10);
    Serial.println("\nConnecting to WiFi...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected!");
}

uint16_t simpleCRC(uint8_t *data, int len) {
    uint16_t crc = 0;
    for (int i = 0; i < len; i++) crc += data[i];
    return crc;
}

bool waitForAck(uint32_t timeout = 3000) { // Tăng timeout lên 3s vì mạng có thể lag
    uint32_t start = millis();
    while (millis() - start < timeout) {
        if (Serial2.available()) {
            uint8_t resp = Serial2.read();

            Serial.printf("[STM32 Response]: 0x%02X\n", resp); // LOG DEBUG
            if (resp == ACK) return true;
            if (resp == NACK) {
                Serial.println("STM32 sent NACK!");
                return false;
            }
    }

    }
    Serial.println("Timeout waiting for STM32...");
    return false;
}

void startStreamingOTA(String url) {
    Serial.println("\n[OTA] --- START ---");
    
    // --- BƯỚC 1: ĐỒNG BỘ TRẠNG THÁI ---
    // Kiểm tra xem STM32 đã ở Bootloader chưa (dựa vào heartbeat nhận được trong loop)
    // Nếu trong 2 giây qua không thấy heartbeat, coi như đang ở App
    if (millis() - last_heartbeat > 2000) {
        Serial.println("[OTA] STM32 is in App. Sending Reset Command (0x7F)...");
        Serial2.write(0x7F); // Lệnh reset từ App sang Boot
        
        // Đợi STM32 khởi động lại và phát ra byte Heartbeat 0xBC
        uint32_t wait_start = millis();
        bool found_boot = false;
        while (millis() - wait_start < 4000) { // Đợi tối đa 4s
            if (Serial2.available() && Serial2.read() == BOOT_HEARTBEAT) {
                Serial.println("[OTA] STM32 Bootloader detected!");
                found_boot = true;
                break;
            }
            delay(10);
        }
        
        if (!found_boot) {
            Serial.println("[OTA] Error: STM32 did not enter Bootloader. Aborting.");
            return;
        }
    } else {
        Serial.println("[OTA] STM32 is already in Bootloader.");
    }

    // --- BƯỚC 2: XÓA BỘ ĐỆM VÀ GỬI LỆNH ERASE ---
    delay(200); // Nghỉ ngắn để UART ổn định
    while(Serial2.available()) Serial2.read(); // Xóa sạch rác/heartbeat còn dư

    Serial.println("[OTA] Sending Erase Command (0xAAFFFF)...");
    uint8_t syncPacket[3] = {START_BYTE, 0xFF, 0xFF};
    bool erase_ok = false;
    
    // for(int i=0; i<3; i++) { // Thử gửi lệnh Erase 3 lần
    //     Serial2.write(syncPacket, 3);
    //     if (waitForAck(5000)) { // Erase tốn thời gian nên đợi 5s
    //         erase_ok = true;
    //         break;
    //     }
    //     Serial.printf("[OTA] Erase attempt %d failed. Retrying...\n", i+1);
    //     delay(500);
    // }

    // if (!erase_ok) {
    //     Serial.println("[OTA] Error: STM32 failed to Erase Flash.");
    //     return;
    // }
    Serial2.write(syncPacket, 3);
    
    if (!waitForAck(5000)) {
        Serial.println("[OTA] Erase Failed.");
        return;
    }
    Serial.println("[OTA] Erase OK. Connecting to Server...");

    // --- BƯỚC 3: TẢI FIRMWARE VÀ STREAM ---
    HTTPClient http;
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        Serial.printf("[OTA] HTTP Error: %d (Check your URL!)\n", httpCode);

        int totalLength = http.getSize();
        WiFiClient* stream = http.getStreamPtr();
        uint8_t buffer[BLOCK_SIZE];
        int bytesRead = 0;
        int blockCount = 0;

        Serial.printf("[OTA] Streaming %d bytes to STM32...\n", totalLength);

        while (http.connected() && (bytesRead < totalLength)) {
        // QUAN TRỌNG: Trước khi gửi Data Block, xóa sạch Heartbeat (0xBC) cũ trong buffer
        while(Serial2.available()) Serial2.read(); 

        if (stream->available()) {
            int len = stream->readBytes(buffer, BLOCK_SIZE);
            uint16_t crc = simpleCRC(buffer, len);
            
            uint8_t packet[BLOCK_SIZE + 5];
            packet[0] = START_BYTE;
            packet[1] = (len >> 8) & 0xFF;
            packet[2] = len & 0xFF;
            memcpy(&packet[3], buffer, len);
            packet[3 + len] = (crc >> 8) & 0xFF;
            packet[4 + len] = crc & 0xFF;

            Serial.printf("[OTA] Sending block %d...", bytesRead/BLOCK_SIZE);
            Serial2.write(packet, len + 5);

            if (waitForAck(3000)) {
                bytesRead += len;
                Serial.println(" OK");
            } else {
                Serial.println(" FAILED (No ACK)");
                http.end();
                return;
            }
        }
        }
        
        // while (http.connected() && (bytesRead < totalLength)) {
        //   // QUAN TRỌNG: Trước khi gửi Data Block, xóa sạch Heartbeat (0xBC) cũ trong buffer
        //   while(Serial2.available()) Serial2.read(); 

        //   if (stream->available()) {
        //         int len = stream->readBytes(buffer, BLOCK_SIZE);
        //         uint16_t crc = simpleCRC(buffer, len);
                
        //         // Đóng gói packet: [0xAA][LenH][LenL][Data][CRCH][CRCL]
        //         uint8_t packet[BLOCK_SIZE + 5];
        //         packet[0] = START_BYTE;
        //         packet[1] = (len >> 8) & 0xFF;
        //         packet[2] = len & 0xFF;
        //         memcpy(&packet[3], buffer, len);
        //         packet[3 + len] = (crc >> 8) & 0xFF;
        //         packet[4 + len] = crc & 0xFF;

        //         // Gửi block với cơ chế Retry
        //         int retry = 3;
        //         bool success = false;
        //         while (retry--) {
        //             Serial2.write(packet, len + 5);
                    
        //             if (waitForAck(3000)) { // Đợi ACK cho mỗi block
        //                 success = true;
        //                 break;
        //             }
        //             Serial.print("!"); // Dấu hiệu retry block
        //         }

        //         if (!success) {
        //             Serial.println("\n[OTA] Failed to send block. Connection lost.");
        //             http.end();
        //             return;
        //         }
                
        //         bytesRead += len;
        //         blockCount++;
        //         if(blockCount % 10 == 0) { // Cứ 10 block in tiến độ 1 lần
        //             Serial.printf("[OTA] Progress: %d%%\n", (bytesRead * 100) / totalLength);
        //         }
        //     }
        // }
        
        // --- BƯỚC 4: KẾT THÚC ---
        Serial.println("\n[OTA] All blocks sent. Sending End Packet...");
        uint8_t endPacket[3] = {START_BYTE, 0x00, 0x00};
        Serial2.write(endPacket, 3);
        
        if(waitForAck(2000)) {
            Serial.println("[OTA] SUCCESS! STM32 is restarting into App.");
        } else {
            Serial.println("[OTA] End Packet not ACKed, but transfer was complete.");
        }
    } 
    else {
        Serial.printf("[OTA] HTTP Error: %d\n", httpCode);
    }
    http.end();
}

// ================= XỬ LÝ SỰ KIỆN MQTT =================
void callback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (int i = 0; i < length; i++) message += (char)payload[i];
    
    Serial.printf("Message arrived on topic: %s\n", topic);
    Serial.printf("Payload: %s\n", message.c_str());

    // Nếu nhận được URL ở topic "device/ota"
    if (String(topic) == "device/ota") {
        Serial.println("--> TRIGGER OTA UPDATE <--");
        startStreamingOTA(message);
    }
}

void reconnect() {
    while (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        String clientId = "ESP32Client-" + String(random(0xffff), HEX);
        
        if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
            Serial.println("connected to HiveMQ!");
            // Đăng ký nhận lệnh OTA
            client.subscribe("device/ota");
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" try again in 5 seconds");
            delay(5000);
        }
    }
}

void setup() {
    Serial.begin(9600);
    Serial2.begin(9600); // UART kết nối với STM32 (chỉnh lại baudrate cho khớp)
    dht.begin();

    setup_wifi();
    
    // Bỏ qua kiểm tra chứng chỉ SSL/TLS để dùng được port 8883 dễ dàng
    espClient.setInsecure(); 
    
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);
}

void loop() {
    if (!client.connected()) reconnect();
    client.loop();

    // --- BỔ SUNG: Xử lý UART từ STM32 ---
    if (Serial2.available()) {
        uint8_t inByte = Serial2.read();

        if (inByte == BOOT_HEARTBEAT) { // Nhận Heartbeat từ Bootloader
            
            last_heartbeat = millis();
            is_stm32_in_boot = true;
        } 
        else if (inByte == 0x01) { // Nhận dữ liệu cảm biến từ App
            uint8_t data[5];
            if (Serial2.readBytes(data, 5) == 5) {
                // Kiểm tra checksum packet[5] ở đây nếu cần
                float t = data[0] + (data[1] / 100.0);
                float h = data[2] + (data[3] / 100.0);
                client.publish("sensor/temperature", String(t).c_str());
                client.publish("sensor/humidity", String(h).c_str());
                is_stm32_in_boot = false;
            }
        }
    }
}