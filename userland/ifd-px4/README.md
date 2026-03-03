# PX4 Smart Card Reader Support

このディレクトリには、px4_drvドライバーのスマートカード機能を`pcscd`（PC/SC daemon）から利用するためのIFDハンドラーが含まれています。

## 概要

px4_drvは、ISDB-T/S（デジタル放送）チューナーのスマートカード機能をLinuxのPC/SC標準インターフェース経由で公開します。

- **カーネルドライバ**: `/dev/px4card*` デバイスノードを提供
- **IFDハンドラー**: pcscd経由でアプリケーションから利用可能
- **対応デバイス**: PX4, PX-MLT, ISDB2056, PX-M1UR, PX-S1UR等、全てのカードリーダー搭載デバイス

## ビルド

### 必要なパッケージ

```bash
# Debian/Ubuntu
sudo apt-get install build-essential libpcsclite-dev pcscd pcsc-tools

# Fedora/RHEL
sudo dnf install gcc make pcsc-lite-devel pcsc-lite pcsc-tools
```

### IFDハンドラーのビルド

```bash
cd userland/ifd-px4
make
```

### インストール

```bash
cd userland/ifd-px4
sudo make install

# pcscd設定

# 使うチューナーのコメント解除
vi px4card.conf

# コピー
sudo install -m 644 px4card.conf /etc/reader.conf.d/
```

## 使い方

### 1. カーネルドライバのロード

```bash
# ドライバをロード
sudo modprobe px4_drv

# デバイスノードの確認
ls -l /dev/px*
# 出力例: crw-rw-rw- 1 root video 249, 0 Mar  3 12:34 /dev/px4card0
```

### 2. pcscdの起動

```bash
# pcscdを再起動
sudo systemctl restart pcscd

# またはデバッグモードで起動
sudo killall pcscd
sudo pcscd -f -d -a
```

### 3. カードリーダーの確認

```bash
# リーダーとカードの認識確認
pcsc_scan
```

出力例：
```
PC/SC device scanner
V 1.6.2 (c) 2001-2022, Ludovic Rousseau <ludovic.rousseau@free.fr>
Using reader plug'n play mechanism
Scanning present readers...
0: PX4 Smart Card Reader #0

Card state: Card inserted
ATR: 3B 8F 80 01 ...
```

### 4. APDUコマンドの送信

```bash
# opensc-toolでカード情報を取得
opensc-tool --reader 0 --send-apdu 00:A4:04:00

# pcsc_scanで継続的にモニタリング
pcsc_scan
```

## トラブルシューティング

### デバイスが認識されない

```bash
# カーネルログを確認
dmesg | grep px4

# デバイスファイルの存在確認
ls -l /dev/px*

# カードが挿入されているか確認（ioctlテスト）
sudo cat /dev/px4card0 | hexdump -C
```

### pcscdがリーダーを認識しない

```bash
# 設定ファイルの確認
cat /etc/reader.conf.d/px4card.conf

# IFDハンドラーのインストール確認
ls -l /usr/lib/pcsc/drivers/ifd-px4.bundle/Contents/Linux/libpx4ifd.so

# pcscdをデバッグモードで起動
sudo pcscd -f -d -a
```

### カードが検出されない

```bash
# カード挿入状態の確認（ioctl直接呼び出し）
# 簡易テストプログラムが必要な場合は別途作成
```

## 技術情報

### アーキテクチャ

```
[アプリケーション (opensc-tool等)]
         ↓ PC/SC API
    [pcscd デーモン]
         ↓ IFD Handler API
  [libpx4ifd.so] (このディレクトリ)
         ↓ ioctl
   [px4_card.ko] (カーネルドライバ)
         ↓ UART
   [IT930x ブリッジ]
         ↓
   [スマートカード]
```

### 対応プロトコル

- **ISO/IEC 7816-3**: 物理・電気特性
- **T=0**: バイト指向プロトコル
- **T=1**: ブロック指向プロトコル（B-CAS標準）
- **APDU**: アプリケーションプロトコルデータユニット

### ボーレート

- 初期: 9600 bps
- 切替可能: 19200 bps (PPS経由)

## 制限事項

- 1デバイスにつき1スロットのみ対応
- ホットプラグは部分的対応（デバイスの抜き差しには未対応）
- 複数カードの同時アクセスは排他制御あり

## ライセンス

GPL v2（カーネルドライバと同じ）

## 参考情報

- PC/SC Workgroup: https://pcscworkgroup.com/
- ISO/IEC 7816-3: Smart card physical characteristics
- pcscd: https://pcsclite.apdu.fr/
