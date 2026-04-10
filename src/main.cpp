/*
 * ESP32-CAM Stream Server
 * PlatformIO / VSCode プロジェクト
 *
 * ■ 書き込み手順
 *   1. IO0とGNDをジャンパーでショート（書き込みモード）
 *   2. VSCodeのPlatformIO: Upload を実行
 *   3. 「SUCCESS」が出たらジャンパーを外す
 *   4. RESETボタンを押下
 *   5. PlatformIO Serial Monitor（115200bps）でIPアドレスを確認
 *
 * ■ SDカード（config.txt）
 *   SDカードのルートに config.txt を作成して各台のIPを設定する
 *   config.txtが無い場合はコード内のデフォルト値で起動する
 *
 * ■ 台ごとの設定変更方法（推奨）
 *   config.txt の ip= の値を変更する（コード書き換え不要）
 *
 * ■ フラッシュLED（GPIO4）注意事項
 *   撮影フラッシュ専用。電流制限抵抗なし。
 *   連続点灯1秒以内厳守。過熱・焼損リスクあり。
 *   SDカードはSPIモード（GPIO13/14/15/2）のためGPIO4競合なし。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <vector>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>

// ============================================================
// カメラピン定義（AI-Thinker ESP32-CAM 固定値）
// ============================================================
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// フラッシュLED ピン
#define FLASH_GPIO_NUM   4

// SDカード SPI ピン（AI-Thinker ESP32-CAM）
#define SD_CS    13
#define SD_MOSI  15
#define SD_CLK   14
#define SD_MISO   2

static SPIClass sdSPI(HSPI);

// ============================================================
// ★ config.txtが無い場合のフォールバック値
// ★ ip は台ごとに変更すること
// ============================================================
static String DEF_IP       = "192.168.1.141";
static String DEF_GATEWAY  = "192.168.1.254";
static String DEF_SUBNET   = "255.255.255.0";
static String DEF_DNS      = "192.168.1.254";
static String DEF_SSID     = "esmcnt";   // ★ここを変更
static String DEF_PASSWORD = "hino4101"; // ★ここを変更

// ============================================================
// グローバル変数
// ============================================================
// PCから受信した現在時刻（YYYYMMDDHHmmss形式）
String currentTime = "";

// 設定値（config.txtまたはデフォルト値で初期化）
String cfgIP       = DEF_IP;
String cfgGateway  = DEF_GATEWAY;
String cfgSubnet   = DEF_SUBNET;
String cfgDns      = DEF_DNS;
String cfgSSID     = DEF_SSID;
String cfgPassword = DEF_PASSWORD;

// HTTPサーバーハンドル（port 80: API専用）
httpd_handle_t camHttpd = NULL;

// ============================================================
// ストリームブロードキャスト（port 81, rawソケット）
// 1タスクがフレームを取得して全クライアントに一斉送信する
// fb_count に依存しないため接続数の制限がなくなる
// ============================================================
#define STREAM_PORT        81
#define MAX_STREAM_CLIENTS 8

static int               streamFds[MAX_STREAM_CLIENTS];
static SemaphoreHandle_t streamMutex = NULL;

static const char STREAM_HTTP_HDR[] =
    "HTTP/1.1 200 OK\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Content-Type: multipart/x-mixed-replace;boundary=MJPEG\r\n"
    "Cache-Control: no-cache\r\n"
    "\r\n";
static const char MJPEG_PART_HDR[] = "--MJPEG\r\nContent-Type: image/jpeg\r\n\r\n";
static const char MJPEG_PART_END[] = "\r\n";

// 接続受付タスク
static void streamAcceptTask(void*) {
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(STREAM_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(serverFd, (struct sockaddr*)&addr, sizeof(addr));
    listen(serverFd, MAX_STREAM_CLIENTS);

    while (true) {
        int clientFd = accept(serverFd, NULL, NULL);
        if (clientFd < 0) continue;

        // 送信タイムアウト設定（遅いクライアントで他をブロックしない）
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // HTTPリクエストを読み捨て
        char buf[256];
        recv(clientFd, buf, sizeof(buf) - 1, 0);

        // HTTPレスポンスヘッダー送信
        send(clientFd, STREAM_HTTP_HDR, strlen(STREAM_HTTP_HDR), 0);

        // クライアント登録
        bool registered = false;
        xSemaphoreTake(streamMutex, portMAX_DELAY);
        for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
            if (streamFds[i] < 0) {
                streamFds[i] = clientFd;
                registered = true;
                Serial.printf("[STREAM] 接続 slot=%d\n", i);
                break;
            }
        }
        xSemaphoreGive(streamMutex);

        if (!registered) {
            close(clientFd);
            Serial.println("[STREAM] 上限(" + String(MAX_STREAM_CLIENTS) + "台)に達しました");
        }
    }
}

// ブロードキャストタスク
static void streamBroadcastTask(void*) {
    while (true) {
        // クライアントがいない間はカメラを使わない
        bool hasClients = false;
        xSemaphoreTake(streamMutex, portMAX_DELAY);
        for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
            if (streamFds[i] >= 0) { hasClients = true; break; }
        }
        xSemaphoreGive(streamMutex);

        if (!hasClients) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        xSemaphoreTake(streamMutex, portMAX_DELAY);
        for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
            if (streamFds[i] < 0) continue;
            int fd = streamFds[i];
            bool ok = send(fd, MJPEG_PART_HDR, strlen(MJPEG_PART_HDR), 0) > 0;
            ok = ok && send(fd, (char*)fb->buf, fb->len, 0) > 0;
            ok = ok && send(fd, MJPEG_PART_END, strlen(MJPEG_PART_END), 0) > 0;
            if (!ok) {
                close(fd);
                streamFds[i] = -1;
                Serial.printf("[STREAM] 切断 slot=%d\n", i);
            }
        }
        xSemaphoreGive(streamMutex);

        esp_camera_fb_return(fb);
    }
}

void startStreamServer() {
    streamMutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) streamFds[i] = -1;
    xTaskCreate(streamAcceptTask,    "stream_accept", 4096, NULL, 5, NULL);
    xTaskCreate(streamBroadcastTask, "stream_bcast",  4096, NULL, 5, NULL);
    Serial.println("[OK] ストリームサーバー起動 (port 81, broadcast)");
}

// ============================================================
// 文字列ユーティリティ：前後の空白を除去する
// ============================================================
static String trimString(const String &s) {
    int start = 0;
    int end = s.length() - 1;
    while (start <= end && isspace(s.charAt(start))) start++;
    while (end >= start && isspace(s.charAt(end))) end--;
    return s.substring(start, end + 1);
}

// ============================================================
// config.txt を読み込んで設定値を更新する
// 失敗した場合はデフォルト値のまま続行する
// ============================================================
void loadConfig() {
    sdSPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sdSPI, 4000000)) {
        Serial.println("[WARN] SDカードマウント失敗 → デフォルト値で起動");
        return;
    }

    // SDカード情報をシリアル出力
    uint8_t cardType = SD.cardType();
    const char* typeStr = (cardType == CARD_MMC)  ? "MMC"  :
                          (cardType == CARD_SD)   ? "SD"   :
                          (cardType == CARD_SDHC) ? "SDHC" : "UNKNOWN";
    uint64_t cardMB = SD.cardSize() / (1024 * 1024);
    Serial.printf("[SD] マウント成功 タイプ:%s 容量:%lluMB\n", typeStr, cardMB);

    File f = SD.open("/config.txt", FILE_READ);
    if (!f) {
        Serial.println("[WARN] config.txt読み込み失敗 → デフォルト値で起動");
        return;
    }

    bool parseOk = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line = trimString(line);

        // コメント行・空行をスキップ
        if (line.length() == 0 || line.startsWith("#")) continue;

        int sep = line.indexOf('=');
        if (sep < 0) continue;

        String key = trimString(line.substring(0, sep));
        String val = trimString(line.substring(sep + 1));

        if      (key == "ip")       { cfgIP       = val; parseOk = true; }
        else if (key == "gateway")  { cfgGateway  = val; }
        else if (key == "subnet")   { cfgSubnet   = val; }
        else if (key == "dns")      { cfgDns      = val; }
        else if (key == "ssid")     { cfgSSID     = val; }
        else if (key == "password") { cfgPassword = val; }
    }
    f.close();

    if (!parseOk) {
        Serial.println("[WARN] config.txt読み込み失敗 → デフォルト値で起動");
    } else {
        Serial.println("[OK] config.txt 読み込み完了");
    }
}

// ============================================================
// WiFiを固定IPで接続する
// 接続失敗時はシリアル出力後に停止する
// ============================================================
void connectWiFi() {
    IPAddress ip, gateway, subnet, dns;
    ip.fromString(cfgIP);
    gateway.fromString(cfgGateway);
    subnet.fromString(cfgSubnet);
    dns.fromString(cfgDns);

    // STAモードを明示してから固定IP・接続開始
    WiFi.mode(WIFI_STA);
    WiFi.config(ip, gateway, subnet, dns);
    WiFi.begin(cfgSSID.c_str(), cfgPassword.c_str());

    Serial.println("[INFO] SSID: " + cfgSSID);
    Serial.print("[INFO] WiFi接続中");
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500);
        Serial.print(".");
        retry++;
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[ERROR] WiFi接続失敗 SSID=" + cfgSSID);
        delay(3000);
        esp_restart();
    }
    Serial.println("[OK] WiFi接続: " + WiFi.localIP().toString());
}

// ============================================================
// カメラを初期化する（OV2640 / UXGA / JPEG品質12）
// バッファはUXGA(1600×1200)サイズで確保し、起動後にSVGAへ下げる
// こうすることで実行時のset_framesizeが安全に動作する
// ============================================================
void initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_UXGA;  // 1600×1200でバッファ確保
    config.jpeg_quality = 10;              // 0〜63（小さいほど高品質）
    config.fb_count     = 2;              // 2バッファでストリームと撮影が競合しない

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[ERROR] カメラ初期化失敗: 0x%x\n", err);
        while (true) { delay(1000); }
    }

    // ストリーム用にSVGA(800×600)へ下げる
    // バッファはUXGA確保済みのため安全に切り替え可能
    sensor_t *s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_SVGA);

    Serial.println("[OK] カメラ初期化完了");
}

// ============================================================
// /photo/YYYYMM/ サブフォルダを再帰的に走査してファイル一覧を降順で返す
// 返す値は "YYYYMM/YYYYMMDD_HHmmss.jpg" 形式（サブフォルダ名込み）
// ============================================================
static std::vector<String> listPhotoFiles() {
    std::vector<String> files;

    File root = SD.open("/photo");
    if (!root || !root.isDirectory()) return files;

    // 年月フォルダを走査
    File monthEntry = root.openNextFile();
    while (monthEntry) {
        if (monthEntry.isDirectory()) {
            String monthName = String(monthEntry.name());
            int slash = monthName.lastIndexOf('/');
            if (slash >= 0) monthName = monthName.substring(slash + 1);

            File photoEntry = monthEntry.openNextFile();
            while (photoEntry) {
                if (!photoEntry.isDirectory()) {
                    String name = String(photoEntry.name());
                    int s = name.lastIndexOf('/');
                    if (s >= 0) name = name.substring(s + 1);
                    if (name.endsWith(".jpg") || name.endsWith(".JPG")) {
                        files.push_back(monthName + "/" + name);
                    }
                }
                // openNextFile() はディレクトリオブジェクトに対して呼ぶ
                // photoEntry.openNextFile() は誤り（ファイルに呼んでも進まない）
                photoEntry = monthEntry.openNextFile();
            }
        }
        monthEntry = root.openNextFile();
    }

    // 降順ソート（新しいファイルが先頭）
    std::sort(files.begin(), files.end(), [](const String &a, const String &b) {
        return a > b;
    });

    return files;
}

// ============================================================
// 年月フォルダを作成して返す（/photo/YYYYMM/）
// currentTimeが未設定の場合は /photo/unknown/ を使う
// ============================================================
static String ensureMonthDir() {
    String dir;
    if (currentTime.length() == 14) {
        dir = "/photo/" + currentTime.substring(0, 6); // YYYYMM
    } else {
        dir = "/photo/unknown";
    }
    if (!SD.exists("/photo")) SD.mkdir("/photo");
    if (!SD.exists(dir))     SD.mkdir(dir);
    return dir;
}

// ============================================================
// 重複しない連番ファイル名を生成する（photo_001.jpg 形式）
// ============================================================
static String generateSeqFileName() {
    String dir = ensureMonthDir();
    for (int i = 1; i <= 9999; i++) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s/photo_%03d.jpg", dir.c_str(), i);
        if (!SD.exists(buf)) {
            return String(buf);
        }
    }
    return dir + "/photo_unknown.jpg";
}



// ============================================================
// 時刻設定ハンドラ（GET /settime?t=YYYYMMDDHHmmss）
// PCの現在時刻を受け取りグローバル変数に保存する
// ============================================================
static esp_err_t setTimeHandler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET");
    httpd_resp_set_type(req, "application/json");

    // クエリ文字列を取得
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"no query\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // パラメータ t を抽出
    char tParam[32] = {0};
    if (httpd_query_key_value(query, "t", tParam, sizeof(tParam)) != ESP_OK) {
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"missing t\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // 14桁チェック
    if (strlen(tParam) != 14) {
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"invalid format\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    currentTime = String(tParam);
    Serial.println("[TIME] 時刻設定: " + currentTime);

    String resp = "{\"status\":\"ok\",\"time\":\"" + currentTime + "\"}";
    httpd_resp_send(req, resp.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================
// 静止画撮影・保存ハンドラ（GET /capture?flash=on|off&t=YYYYMMDDHHmmss）
// t パラメータで時刻を同時受信することで /settime との2往復を1往復に削減する
// ============================================================
static esp_err_t captureHandler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET");
    httpd_resp_set_type(req, "application/json");

    char query[128] = {0};
    bool useFlash = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        // flash パラメータ
        char flashParam[16] = {0};
        if (httpd_query_key_value(query, "flash", flashParam, sizeof(flashParam)) == ESP_OK) {
            useFlash = (strcmp(flashParam, "on") == 0);
        }
        // t パラメータ（時刻）— /settime を省略するための統合パラメータ
        char tParam[32] = {0};
        if (httpd_query_key_value(query, "t", tParam, sizeof(tParam)) == ESP_OK
            && strlen(tParam) == 14) {
            currentTime = String(tParam);
        }
    }

    // 年月フォルダを自動作成（/photo/YYYYMM/）
    String monthDir = ensureMonthDir();

    // 撮影前: センサーを高解像度(UXGA 1600×1200)に切り替え
    sensor_t *sensor = esp_camera_sensor_get();
    sensor->set_framesize(sensor, FRAMESIZE_UXGA);
    // センサーが新解像度に安定するまで待機（バッファをフラッシュ）
    delay(200);
    camera_fb_t *dummy = esp_camera_fb_get();
    if (dummy) esp_camera_fb_return(dummy);

    camera_fb_t *fb = nullptr;

    // フラッシュON撮影フロー
    if (useFlash) {
        digitalWrite(FLASH_GPIO_NUM, HIGH); // フラッシュ点灯
        delay(100);                          // センサー露光安定待ち
        fb = esp_camera_fb_get();            // 撮影
        digitalWrite(FLASH_GPIO_NUM, LOW);   // 即座に消灯（1秒以内厳守）
    } else {
        fb = esp_camera_fb_get();
    }

    // 撮影後: ストリーム用解像度(SVGA 800×600)に戻す
    sensor->set_framesize(sensor, FRAMESIZE_SVGA);

    if (!fb) {
        digitalWrite(FLASH_GPIO_NUM, LOW);
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"camera capture failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // ファイル名を決定する（/photo/YYYYMM/YYYYMMDD_HHmmss.jpg）
    String filePath;
    if (currentTime.length() == 14) {
        // 時刻設定済み: /photo/YYYYMM/YYYYMMDD_HHmmss.jpg
        String dateStr = currentTime.substring(0, 8);
        String timeStr = currentTime.substring(8, 14);
        filePath = monthDir + "/" + dateStr + "_" + timeStr + ".jpg";
    } else {
        // 時刻未設定: /photo/unknown/photo_001.jpg
        filePath = generateSeqFileName();
    }

    // SDカードに保存
    File photoFile = SD.open(filePath, FILE_WRITE);
    if (!photoFile) {
        esp_camera_fb_return(fb);
        digitalWrite(FLASH_GPIO_NUM, LOW); // 念のため消灯
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"SD write failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    size_t written = photoFile.write(fb->buf, fb->len);
    photoFile.close();
    esp_camera_fb_return(fb); // メモリ解放
    // 念のためフラッシュ消灯確認（finally相当）
    digitalWrite(FLASH_GPIO_NUM, LOW);

    if (written == 0) {
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"SD write failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    Serial.println("[CAPTURE] 保存: " + filePath);

    // レスポンス: "/photo/" を除いた相対パスを返す（HTMLのfile引数に直接使える形式）
    String relPath = filePath.substring(7); // "/photo/YYYYMM/file.jpg" → "YYYYMM/file.jpg"
    String flashStr = useFlash ? "on" : "off";
    String resp = "{\"status\":\"ok\",\"file\":\"" + relPath + "\",\"flash\":\"" + flashStr + "\"}";
    httpd_resp_send(req, resp.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================
// 写真一覧取得ハンドラ（GET /photos）
// チャンク送信でヒープ使用量を抑える
// ============================================================
static esp_err_t photosHandler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET");
    httpd_resp_set_type(req, "application/json");

    std::vector<String> files = listPhotoFiles();

    // チャンク送信: 巨大なStringを1つ作らず1エントリずつ送る
    httpd_resp_send_chunk(req, "{\"files\":[", 10);
    for (size_t i = 0; i < files.size(); i++) {
        String entry = (i > 0 ? "," : "");
        entry += "\"" + files[i] + "\"";
        httpd_resp_send_chunk(req, entry.c_str(), entry.length());
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ============================================================
// URLデコード（%2F→/ など）
// httpd_query_key_value はデコードしないため手動で処理する
// ============================================================
static String urlDecode(const char* src) {
    String result;
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            result += (char)strtol(hex, nullptr, 16);
            src += 3;
        } else {
            result += *src++;
        }
    }
    return result;
}

// ============================================================
// 写真取得ハンドラ（GET /photo?file=YYYYMM/YYYYMMDD_HHmmss.jpg）
// 指定ファイルをSDカードから読み込んでバイナリ送信する
// ============================================================
static esp_err_t photoHandler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET");

    // file パラメータを取得
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    char fileParam[64] = {0};
    if (httpd_query_key_value(query, "file", fileParam, sizeof(fileParam)) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    // URLデコード（%2F→/ など）してからパス検証
    String fileName = urlDecode(fileParam);

    // パスインジェクション対策: .. を含むパスを拒否
    // YYYYMM/YYYYMMDD_HHmmss.jpg 形式のサブフォルダパスは許可する
    if (fileName.indexOf("..") >= 0) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    String filePath = "/photo/" + fileName;

    File f = SD.open(filePath, FILE_READ);
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "image/jpeg");

    // チャンク単位で送信
    static uint8_t buf[4096];
    size_t remaining = f.size();
    while (remaining > 0) {
        size_t toRead = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        size_t r = f.read(buf, toRead);
        if (r == 0) break;
        if (httpd_resp_send_chunk(req, (const char*)buf, r) != ESP_OK) break;
        remaining -= r;
    }
    f.close();

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ============================================================
// SDカード・システム状態確認ハンドラ（GET /status）
// ============================================================
static esp_err_t statusHandler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");

    uint8_t cardType = SD.cardType();
    bool sdOk = (cardType != CARD_NONE);

    const char* typeStr = (cardType == CARD_MMC)  ? "MMC"  :
                          (cardType == CARD_SD)   ? "SD"   :
                          (cardType == CARD_SDHC) ? "SDHC" :
                          (cardType == CARD_NONE) ? "NONE" : "UNKNOWN";

    uint64_t cardMB  = sdOk ? SD.cardSize() / (1024 * 1024) : 0;
    uint64_t totalMB = sdOk ? SD.totalBytes() / (1024 * 1024) : 0;
    uint64_t usedMB  = sdOk ? SD.usedBytes()  / (1024 * 1024) : 0;

    char json[256];
    snprintf(json, sizeof(json),
        "{\"sd\":{\"mounted\":%s,\"type\":\"%s\",\"card_mb\":%llu,\"total_mb\":%llu,\"used_mb\":%llu},\"wifi\":\"%s\"}",
        sdOk ? "true" : "false",
        typeStr,
        cardMB, totalMB, usedMB,
        WiFi.localIP().toString().c_str()
    );

    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================
// APIサーバー起動（port 80）
// ストリームは startStreamServer()（port 81, rawソケット）で別途起動
// ============================================================
void startCamServer() {
    // --- APIサーバー (port 80) ---
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 8;
    cfg.max_open_sockets = 5;

    if (httpd_start(&camHttpd, &cfg) != ESP_OK) {
        Serial.println("[ERROR] サーバー起動失敗");
        return;
    }

    httpd_uri_t captureUri = {
        .uri      = "/capture",
        .method   = HTTP_GET,
        .handler  = captureHandler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(camHttpd, &captureUri);

    httpd_uri_t setTimeUri = {
        .uri      = "/settime",
        .method   = HTTP_GET,
        .handler  = setTimeHandler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(camHttpd, &setTimeUri);

    httpd_uri_t photosUri = {
        .uri      = "/photos",
        .method   = HTTP_GET,
        .handler  = photosHandler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(camHttpd, &photosUri);

    httpd_uri_t photoUri = {
        .uri      = "/photo",
        .method   = HTTP_GET,
        .handler  = photoHandler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(camHttpd, &photoUri);

    httpd_uri_t statusUri = {
        .uri      = "/status",
        .method   = HTTP_GET,
        .handler  = statusHandler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(camHttpd, &statusUri);

    Serial.println("[OK] APIサーバー起動 (port 80)");
}

// ============================================================
// 起動時の初期化処理
// ============================================================
void setup() {
    // 1. シリアル初期化
    Serial.begin(115200);
    Serial.println("\n[INFO] 起動中...");

    // 2. GPIO4（フラッシュLED）を出力・消灯に初期化
    pinMode(FLASH_GPIO_NUM, OUTPUT);
    digitalWrite(FLASH_GPIO_NUM, LOW);

    // 3. SDカードマウント・config.txt 読み込み
    loadConfig();

    // 4. WiFi 固定IP接続
    connectWiFi();

    // 5. カメラ初期化
    initCamera();

    // 6. APIサーバー起動（port 80）
    startCamServer();

    // 7. ストリームサーバー起動（port 81, rawソケット broadcast）
    startStreamServer();

    // 8. 起動完了・エンドポイント一覧をシリアル出力
    String ip = WiFi.localIP().toString();
    Serial.println("==========================");
    Serial.println(" ESP32-CAM Stream Server");
    Serial.println("==========================");
    Serial.println("[OK] WiFi接続: " + ip);
    Serial.println("[ENDPOINT] Stream  : http://" + ip + ":81/stream");
    Serial.println("[ENDPOINT] Capture : http://" + ip + "/capture?flash=on(off)");
    Serial.println("[ENDPOINT] SetTime : http://" + ip + "/settime?t=YYYYMMDDHHmmss");
    Serial.println("[ENDPOINT] Photos  : http://" + ip + "/photos");
    Serial.println("[ENDPOINT] Photo   : http://" + ip + "/photo?file=ファイル名.jpg");
    Serial.println("[ENDPOINT] Status  : http://" + ip + "/status");
    Serial.println("==========================");
}

// ============================================================
// メインループ（HTTPサーバーがタスクで動作するため何もしない）
// ============================================================
void loop() {
    delay(10000);
}
