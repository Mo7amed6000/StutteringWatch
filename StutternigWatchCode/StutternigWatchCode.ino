#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>
#include <time.h>

// ======================
// Wi‑Fi and API settings
// ======================
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";

const char *tokenURL = "https://YOUR_BACKEND_HOST/api/login/";
const char *uploadURL = "https://YOUR_BACKEND_HOST/api/audio_chunk_upload/";
const char *submitURL = "https://YOUR_BACKEND_HOST/api/audio_chunk_submit/";

const char *email = "device-account@example.com";
const char *apiPassword = "YOUR_DEVICE_PASSWORD";

String currentCreatedAt = "";
unsigned long lastSpeechTime = 0;
bool wasSpeaking = false;

const unsigned long bootEpochMillis = 0;
const char* bootEpochString = "2025-12-31T00:00:00Z";
String authToken = "";

// ======================
// I2S / Audio settings
// ======================
#define SAMPLE_RATE 8000
#define SAMPLE_BUFFER_SIZE 512
#define RECORD_DURATION_MS 3000
#define I2S_MIC_CHANNEL I2S_CHANNEL_FMT_ONLY_LEFT
#define VAD_THRESHOLD 10000000

#define I2S_MIC_SERIAL_CLOCK GPIO_NUM_32
#define I2S_MIC_LEFT_RIGHT_CLOCK GPIO_NUM_25
#define I2S_MIC_SERIAL_DATA GPIO_NUM_33

i2s_config_t i2s_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
  .sample_rate = SAMPLE_RATE,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
  .channel_format = I2S_MIC_CHANNEL,
  .communication_format = I2S_COMM_FORMAT_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 4,
  .dma_buf_len = 1024,
  .use_apll = false,
  .tx_desc_auto_clear = false,
  .fixed_mclk = 0
};

i2s_pin_config_t i2s_mic_pins = {
  .bck_io_num = I2S_MIC_SERIAL_CLOCK,
  .ws_io_num = I2S_MIC_LEFT_RIGHT_CLOCK,
  .data_out_num = I2S_PIN_NO_CHANGE,
  .data_in_num = I2S_MIC_SERIAL_DATA
};

// ======================
// WiFi & Token
// ======================

void connectToWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    WiFi.setSleep(false);
  } else {
    Serial.println("\nFailed to connect! Restarting...");
    ESP.restart();
  }
}

String getToken() {
  WiFiClientSecure secureClient;
  secureClient.setInsecure(); // Skip certificate check

  HTTPClient http;
  http.begin(secureClient, tokenURL);
  http.addHeader("Content-Type", "application/json");

  String jsonBody = "{\"email\":\"" + String(email) + "\",\"password\":\"" + String(apiPassword) + "\"}";
  Serial.println("Requesting token with: " + jsonBody);

  int httpCode = http.POST(jsonBody);
  String token = "";
  if (httpCode == 200) {
    String response = http.getString();
    Serial.println("Token response: " + response);
    int tokenPos = response.indexOf("\"token\":\"");
    if (tokenPos >= 0) {
      tokenPos += 9;
      int endPos = response.indexOf("\"", tokenPos);
      token = response.substring(tokenPos, endPos);
    }
  } else {
    Serial.print("Failed to get token, HTTP code: ");
    Serial.println(httpCode);
  }
  http.end();
  return token;
}

// ======================
// Audio Recording
// ======================

void createWavHeader(uint8_t *header, uint32_t pcmDataLength, uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample) {
  uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
  uint16_t blockAlign = channels * bitsPerSample / 8;
  uint32_t chunkSize = 36 + pcmDataLength;

  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  header[4] = chunkSize & 0xff;
  header[5] = (chunkSize >> 8) & 0xff;
  header[6] = (chunkSize >> 16) & 0xff;
  header[7] = (chunkSize >> 24) & 0xff;
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
  header[20] = 1; header[21] = 0;
  header[22] = channels; header[23] = 0;
  header[24] = sampleRate & 0xff;
  header[25] = (sampleRate >> 8) & 0xff;
  header[26] = (sampleRate >> 16) & 0xff;
  header[27] = (sampleRate >> 24) & 0xff;
  header[28] = byteRate & 0xff;
  header[29] = (byteRate >> 8) & 0xff;
  header[30] = (byteRate >> 16) & 0xff;
  header[31] = (byteRate >> 24) & 0xff;
  header[32] = blockAlign; header[33] = 0;
  header[34] = bitsPerSample; header[35] = 0;
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  header[40] = pcmDataLength & 0xff;
  header[41] = (pcmDataLength >> 8) & 0xff;
  header[42] = (pcmDataLength >> 16) & 0xff;
  header[43] = (pcmDataLength >> 24) & 0xff;
}

bool recordAudioToWav(uint8_t **wavData, size_t *wavSize) {
  uint32_t durationMs = RECORD_DURATION_MS;
  uint32_t startTime = millis();
  uint32_t maxSamples = SAMPLE_RATE * durationMs / 1000;
  size_t pcmBufferSize = maxSamples * sizeof(int16_t);
  int16_t *pcmBuffer = (int16_t *)malloc(pcmBufferSize);
  if (!pcmBuffer) return false;

  uint32_t sampleCount = 0;
  while (millis() - startTime < durationMs && sampleCount < maxSamples) {
    int32_t rawSamples[SAMPLE_BUFFER_SIZE];
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, rawSamples, sizeof(rawSamples), &bytesRead, 100);
    int samplesRead = bytesRead / sizeof(int32_t);
    for (int i = 0; i < samplesRead && sampleCount < maxSamples; i++) {
      pcmBuffer[sampleCount++] = (int16_t)(rawSamples[i] >> 16);
    }
  }

  size_t actualPcmSize = sampleCount * sizeof(int16_t);
  size_t fileSize = 44 + actualPcmSize;
  uint8_t *fileBuffer = (uint8_t *)malloc(fileSize);
  if (!fileBuffer) {
    free(pcmBuffer);
    return false;
  }

  createWavHeader(fileBuffer, actualPcmSize, SAMPLE_RATE, 1, 16);
  memcpy(fileBuffer + 44, pcmBuffer, actualPcmSize);
  free(pcmBuffer);
  *wavData = fileBuffer;
  *wavSize = fileSize;
  return true;
}

bool sendAudioChunk(const String &token, uint8_t *wavData, size_t wavSize, const String &createdAt) {
  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient http;
  http.begin(secureClient, uploadURL);
  http.addHeader("Authorization", "Token " + token);

  String boundary = "----ESP32Boundary";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  String part1 = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"audio_chunk\"; filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String part2 = "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"created_at\"\r\n\r\n" + createdAt + "\r\n";
  String part3 = "--" + boundary + "--\r\n";

  int totalSize = part1.length() + wavSize + part2.length() + part3.length();
  uint8_t *postData = (uint8_t *)malloc(totalSize);
  if (!postData) return false;

  memcpy(postData, part1.c_str(), part1.length());
  memcpy(postData + part1.length(), wavData, wavSize);
  memcpy(postData + part1.length() + wavSize, part2.c_str(), part2.length());
  memcpy(postData + part1.length() + wavSize + part2.length(), part3.c_str(), part3.length());

  int httpResponseCode = http.POST(postData, totalSize);
  Serial.print("Upload HTTP response: ");
  Serial.println(httpResponseCode);
  Serial.println("Response: " + http.getString());

  free(postData);
  http.end();
  return (httpResponseCode >= 200 && httpResponseCode < 300);
}

bool submitAudioChunk(const String &token, const String &timestamp) {
  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient http;
  http.begin(secureClient, submitURL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Token " + token);

  String payload = "{\"created_at\": \"" + timestamp + "\"}";
  int httpCode = http.POST(payload);
  Serial.print("Submit HTTP response: ");
  Serial.println(httpCode);
  Serial.println("Submit response: " + http.getString());
  http.end();
  return (httpCode >= 200 && httpCode < 300);
}

String getCurrentTimestampFromMillis() {
  unsigned long secondsSinceBoot = millis() / 1000;
  struct tm baseTime = {};
  strptime(bootEpochString, "%Y-%m-%dT%H:%M:%SZ", &baseTime);
  time_t baseEpoch = mktime(&baseTime);
  time_t currentEpoch = baseEpoch + secondsSinceBoot;
  struct tm* currentTime = gmtime(&currentEpoch);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", currentTime);
  return String(buffer);
}

void vadAndRecord() {
  int32_t raw_samples[SAMPLE_BUFFER_SIZE];
  size_t bytes_read = 0;
  i2s_read(I2S_NUM_0, raw_samples, sizeof(raw_samples), &bytes_read, portMAX_DELAY);
  int samples_read = bytes_read / sizeof(int32_t);

  bool speechDetected = false;
  for (int i = 0; i < samples_read; i++) {
    if (raw_samples[i] > VAD_THRESHOLD || raw_samples[i] < -VAD_THRESHOLD) {
      speechDetected = true;
      break;
    }
  }

  unsigned long currentMillis = millis();

  if (speechDetected) {
    lastSpeechTime = currentMillis;

    if (!wasSpeaking) {
      currentCreatedAt = getCurrentTimestampFromMillis();
      wasSpeaking = true;
      Serial.println("Voice session started. created_at: " + currentCreatedAt);
    }

    uint8_t *wavData = NULL;
    size_t wavSize = 0;
    if (recordAudioToWav(&wavData, &wavSize)) {
      if (!sendAudioChunk(authToken, wavData, wavSize, currentCreatedAt)) {
        Serial.println("Audio chunk upload failed. Refreshing token...");
        authToken = getToken();
      } else {
        Serial.println("Audio chunk uploaded successfully.");
      }
      free(wavData);
    }

    delay(500);
  } else {
    if (wasSpeaking && currentMillis - lastSpeechTime > 1500) {
      wasSpeaking = false;
      if (!submitAudioChunk(authToken, currentCreatedAt)) {
        Serial.println("Submit failed. Refreshing token...");
        authToken = getToken();
      } else {
        Serial.println("Submit successful.");
      }
      currentCreatedAt = "";
    }
    Serial.print(".");
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  connectToWiFi();
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &i2s_mic_pins);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Reconnecting...");
    connectToWiFi();
  }

  if (authToken == "") {
    authToken = getToken();
    if (authToken == "") {
      Serial.println("No token received. Retrying in 5 seconds...");
      delay(5000);
      return;
    }
    Serial.println("Stored token: " + authToken);
  }

  vadAndRecord();
  delay(100);
}
