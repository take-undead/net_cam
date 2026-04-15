# ESP32-CAM Stream Server

AI-Thinker ESP32-CAM 向けの MJPEG ストリーミング・静止画撮影・SD カード保存サーバーです。
最大 6 台を同時に管理できる PC 用 Web ビューア（`viewer.html`）が付属します。

## 機能

- **MJPEG ライブストリーミング**（ポート 81）：ブロードキャスト方式で最大 8 クライアント同時接続
- **静止画撮影**：UXGA（1600×1200）で撮影し SD カードに保存
- **フラッシュ LED 対応**：撮影時のオン/オフをリクエストで指定
- **年月フォルダ自動作成**：`/photo/YYYYMM/YYYYMMDD_HHmmss.jpg` 形式で保存
- **写真一覧・表示**：ブラウザから保存済み写真を閲覧
- **config.txt 設定**：コード書き換え不要で IP・WiFi を設定
- **固定 IP 接続**：台ごとに異なる IP を割り当て可能

## ハードウェア

| 項目 | 内容 |
|------|------|
| ボード | AI-Thinker ESP32-CAM |
| カメラ | OV2640 |
| SD カード | FAT32 フォーマット（SPI モード） |
| 開発環境 | PlatformIO + VSCode |

### SD カード SPI ピン配置

| 信号 | GPIO |
|------|------|
| CS   | 13   |
| MOSI | 15   |
| CLK  | 14   |
| MISO | 2    |

> SD カードは SDMMC モードではなく **SPI モード**で初期化します。安価な 4GB カードを含む幅広い SD カードに対応します。

## API エンドポイント

| エンドポイント | 説明 |
|---------------|------|
| `http://[IP]:81/stream` | MJPEG ライブストリーム |
| `GET /capture?flash=on\|off&t=YYYYMMDDHHmmss` | 静止画撮影・保存 |
| `GET /photos` | 保存済み写真一覧（JSON） |
| `GET /photo?file=YYYYMM/ファイル名.jpg` | 写真取得 |
| `GET /settime?t=YYYYMMDDHHmmss` | 時刻設定 |
| `GET /status` | SD カード・WiFi 状態確認 |

## SD カード設定ファイル（config.txt）

SD カードのルートに `config.txt` を作成します。ファイルがない場合はコード内のデフォルト値で起動します。

```ini
ip=192.168.1.141
gateway=192.168.1.254
subnet=255.255.255.0
dns=192.168.1.254
ssid=your_wifi_ssid
password=your_wifi_password
```

台ごとに `ip=` の値を変えるだけで設定が完了します。コードの書き換えは不要です。

## 写真の保存パス

```
/photo/
  └── 202604/
        ├── 20260401_120000.jpg
        ├── 20260401_153045.jpg
        └── ...
```

## ファームウェア書き込み手順

1. IO0 と GND をジャンパーでショート（書き込みモード）
2. VSCode の PlatformIO: **Upload** を実行
3. 「SUCCESS」が出たらジャンパーを外す
4. RESET ボタンを押下
5. PlatformIO Serial Monitor（115200 bps）で IP アドレスを確認

## Web ビューア（viewer.html）

`viewer.html` をブラウザで開くと 6 台のカメラを 3×2 グリッドで管理できます。

- 各カメラの IP をブラウザ上で設定（`localStorage` に保存）
- オンライン検出時にストリームを自動再生
- 撮影ボタン・フラッシュ切替
- 右パネルで保存済み写真を閲覧

## カメラ解像度

| 用途 | 解像度 |
|------|--------|
| ストリーミング | SVGA（800×600） |
| 静止画撮影 | UXGA（1600×1200） |

起動時に UXGA でバッファを確保し、ストリーム用に SVGA へ切り替えます。撮影時のみ UXGA に戻し、完了後 SVGA に戻します。

## フラッシュ LED 注意事項

GPIO4 に電流制限抵抗なしで接続されています。**連続点灯 1 秒以内**を厳守してください。過熱・焼損のリスクがあります。
