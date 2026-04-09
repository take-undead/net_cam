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
 *   SDカードは1-bitモード初期化によりGPIO4競合なし。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <SD_MMC.h>
#include <FS.h>
#include <vector>
#include <algorithm>

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

// HTTPサーバーハンドル
httpd_handle_t camHttpd = NULL;

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
    // SDカードを1-bitモードでマウント
    delay(300);
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("[WARN] SDカードマウント失敗 → デフォルト値で起動");
        return;
    }

    File f = SD_MMC.open("/config.txt", FILE_READ);
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
// カメラを初期化する（OV2640 / SVGA / JPEG品質12）
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
    config.frame_size   = FRAMESIZE_SVGA;   // 800×600
    config.jpeg_quality = 12;               // 0〜63（小さいほど高品質）
    config.fb_count     = 2;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[ERROR] カメラ初期化失敗: 0x%x\n", err);
        while (true) { delay(1000); }
    }
    Serial.println("[OK] カメラ初期化完了");
}

// ============================================================
// /photo/ ディレクトリのファイル一覧を降順ソートで返すヘルパー
// ============================================================
static std::vector<String> listPhotoFiles() {
    std::vector<String> files;

    File dir = SD_MMC.open("/photo");
    if (!dir || !dir.isDirectory()) return files;

    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = String(entry.name());
            // パスが含まれる場合はファイル名のみ抽出
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            if (name.endsWith(".jpg") || name.endsWith(".JPG")) {
                files.push_back(name);
            }
        }
        entry = dir.openNextFile();
    }

    // 降順ソート（新しいファイルが先頭）
    std::sort(files.begin(), files.end(), [](const String &a, const String &b) {
        return a > b;
    });

    return files;
}

// ============================================================
// 重複しない連番ファイル名を生成する（photo_001.jpg 形式）
// ============================================================
static String generateSeqFileName() {
    for (int i = 1; i <= 9999; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "/photo/photo_%03d.jpg", i);
        if (!SD_MMC.exists(buf)) {
            return String(buf);
        }
    }
    return "/photo/photo_unknown.jpg";
}

// ============================================================
// MJPEGストリームハンドラ（GET /stream）
// フレームを連続送信し続ける。エラー時はループを抜ける
// ============================================================
static esp_err_t streamHandler(httpd_req_t *req) {
    // CORSヘッダー付与
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET");

    // MJPEGコンテンツタイプ設定
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=BOUNDARY");

    static const char* boundary = "--BOUNDARY\r\nContent-Type: image/jpeg\r\n\r\n";
    static const char* boundaryEnd = "\r\n";

    while (true) {
        // フレームバッファ取得
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[WARN] フレームバッファ取得失敗");
            break;
        }

        // バウンダリヘッダー送信
        esp_err_t res = httpd_resp_send_chunk(req, boundary, strlen(boundary));
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        // JPEGデータ送信
        res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
        esp_camera_fb_return(fb); // 必ずメモリ解放
        if (res != ESP_OK) break;

        // バウンダリ末尾送信
        res = httpd_resp_send_chunk(req, boundaryEnd, strlen(boundaryEnd));
        if (res != ESP_OK) break;
    }

    // チャンク送信終了
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
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
// 静止画撮影・保存ハンドラ（GET /capture?flash=on|off）
// フラッシュ使用有無を指定して撮影しSDカードに保存する
// ============================================================
static esp_err_t captureHandler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET");
    httpd_resp_set_type(req, "application/json");

    // flash パラメータを取得
    char query[64] = {0};
    bool useFlash = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char flashParam[16] = {0};
        if (httpd_query_key_value(query, "flash", flashParam, sizeof(flashParam)) == ESP_OK) {
            useFlash = (strcmp(flashParam, "on") == 0);
        }
    }

    // /photo/ ディレクトリを自動作成
    if (!SD_MMC.exists("/photo")) {
        SD_MMC.mkdir("/photo");
    }

    camera_fb_t *fb = nullptr;

    // フラッシュON撮影フロー
    if (useFlash) {
        digitalWrite(FLASH_GPIO_NUM, HIGH); // フラッシュ点灯
        delay(100);                          // センサー露光安定待ち
        fb = esp_camera_fb_get();            // 撮影
        digitalWrite(FLASH_GPIO_NUM, LOW);   // 即座に消灯（1秒以内厳守）
    } else {
        // フラッシュOFF撮影フロー
        fb = esp_camera_fb_get();
    }

    if (!fb) {
        // エラー時もフラッシュを確実に消灯（finally相当）
        digitalWrite(FLASH_GPIO_NUM, LOW);
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"camera capture failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // ファイル名を決定する
    String filePath;
    if (currentTime.length() == 14) {
        // 時刻設定済み: YYYYMMDDHHmmss → YYYYMMDD_HHmmss.jpg
        String dateStr = currentTime.substring(0, 8);
        String timeStr = currentTime.substring(8, 14);
        filePath = "/photo/" + dateStr + "_" + timeStr + ".jpg";
    } else {
        // 時刻未設定: 連番ファイル名
        filePath = generateSeqFileName();
    }

    // SDカードに保存
    File photoFile = SD_MMC.open(filePath, FILE_WRITE);
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

    // ファイル名のみ（パスなし）をレスポンスに含める
    String fileName = filePath.substring(7); // "/photo/" を除去
    String flashStr = useFlash ? "on" : "off";
    String resp = "{\"status\":\"ok\",\"file\":\"" + filePath + "\",\"flash\":\"" + flashStr + "\"}";
    httpd_resp_send(req, resp.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================
// 写真一覧取得ハンドラ（GET /photos）
// /photo/ ディレクトリのJPEGファイル名を降順で返す
// ============================================================
static esp_err_t photosHandler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET");
    httpd_resp_set_type(req, "application/json");

    std::vector<String> files = listPhotoFiles();

    // JSON配列を構築
    String json = "{\"files\":[";
    for (size_t i = 0; i < files.size(); i++) {
        if (i > 0) json += ",";
        json += "\"" + files[i] + "\"";
    }
    json += "]}";

    httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============================================================
// 写真取得ハンドラ（GET /photo?file=ファイル名.jpg）
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

    // パスインジェクション対策: ファイル名に / や .. が含まれないようにする
    String fileName = String(fileParam);
    if (fileName.indexOf('/') >= 0 || fileName.indexOf("..") >= 0) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    String filePath = "/photo/" + fileName;

    File f = SD_MMC.open(filePath, FILE_READ);
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
// 全エンドポイントを1つのHTTPサーバー（ポート80）で起動する
// メモリ節約のため2サーバー構成をやめて統合する
// ============================================================
void startCamServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 3; // メモリ節約

    if (httpd_start(&camHttpd, &config) != ESP_OK) {
        Serial.println("[ERROR] サーバー起動失敗");
        return;
    }

    httpd_uri_t streamUri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = streamHandler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(camHttpd, &streamUri);

    httpd_uri_t captureUri = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = captureHandler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(camHttpd, &captureUri);

    httpd_uri_t setTimeUri = {
        .uri       = "/settime",
        .method    = HTTP_GET,
        .handler   = setTimeHandler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(camHttpd, &setTimeUri);

    httpd_uri_t photosUri = {
        .uri       = "/photos",
        .method    = HTTP_GET,
        .handler   = photosHandler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(camHttpd, &photosUri);

    httpd_uri_t photoUri = {
        .uri       = "/photo",
        .method    = HTTP_GET,
        .handler   = photoHandler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(camHttpd, &photoUri);

    Serial.println("[OK] サーバー起動 (port 80)");
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

    // 6. HTTPサーバー起動（ポート80に全エンドポイント統合）
    startCamServer();

    // 7. 起動完了・エンドポイント一覧をシリアル出力
    String ip = WiFi.localIP().toString();
    Serial.println("==========================");
    Serial.println(" ESP32-CAM Stream Server");
    Serial.println("==========================");
    Serial.println("[OK] WiFi接続: " + ip);
    Serial.println("[ENDPOINT] Stream  : http://" + ip + "/stream");
    Serial.println("[ENDPOINT] Capture : http://" + ip + "/capture?flash=on(off)");
    Serial.println("[ENDPOINT] SetTime : http://" + ip + "/settime?t=YYYYMMDDHHmmss");
    Serial.println("[ENDPOINT] Photos  : http://" + ip + "/photos");
    Serial.println("[ENDPOINT] Photo   : http://" + ip + "/photo?file=ファイル名.jpg");
    Serial.println("==========================");
}

// ============================================================
// メインループ（HTTPサーバーがタスクで動作するため何もしない）
// ============================================================
void loop() {
    delay(10000);
}
