# PX4 Smart Card Reader Support

このディレクトリには、px4_drvドライバーのスマートカード機能を`pcscd`（PC/SC daemon）から利用するためのIFDハンドラーが含まれています。

## 概要

px4_drvは、ISDB-T/S（デジタル放送）チューナーのスマートカード機能をLinuxのPC/SC標準インターフェース経由で公開します。

- **カーネルドライバ**: `/dev/px4card*` デバイスノードを提供
- **IFDハンドラー**: pcscd経由でアプリケーションから利用可能
- **対応デバイス**: PX4, PX-MLT, ISDB2056, PX-M1UR, PX-S1UR等、全てのカードリーダー搭載デバイス

## Debian パッケージを使用してインストール

Debian / Ubuntu では、リリース済みの Debian パッケージから IFD ハンドラーを
インストールできます。`apt` を使うと、`pcscd` などの依存パッケージも自動的に導入されます。
カード機能を提供する px4_drv の導入手順は、[ルートの README](../../README.md#2-ドライバのインストール)を参照してください。

アーキテクチャは `dpkg --print-architecture` で確認できます。表示された値に対応する
次のいずれかの手順を実行してください。

### amd64 の場合

```bash
wget https://github.com/yyya-nico/px4_drv/releases/download/v0.6.1/px4-ifd_0.6.1_amd64.deb
sudo apt install -y ./px4-ifd_0.6.1_amd64.deb
```

### arm64 の場合

```bash
wget https://github.com/yyya-nico/px4_drv/releases/download/v0.6.1/px4-ifd_0.6.1_arm64.deb
sudo apt install -y ./px4-ifd_0.6.1_arm64.deb
```

> [!TIP]
> `px4-drv-dkms` は関連パッケージの案内（`Suggests`）に含まれるため、APT の通常設定では
> 自動導入されません。カード機能対応のドライバを DKMS などで手動導入済みの場合も、
> IFD ハンドラーだけを追加できます。

導入後のカードリーダーの確認方法は、[使い方](#使い方)を参照してください。
`pcsc_scan` を利用する場合は、`sudo apt install pcsc-tools` で追加できます。
既に pcscd が動いている場合は、カードを使用する録画などを終了した後で
`sudo systemctl restart pcscd` を実行してください。

### Docker コンテナで利用する場合

Docker ではホストのカーネルを共有するため、px4_drv の導入とロードはホスト側で必要です。
コンテナ内への `px4-drv-dkms` の導入は不要で、イメージ作成時に上記の `.deb` から
IFD ハンドラーと依存する `pcscd` を導入できます。

コンテナ起動時には、利用するカードデバイスを `--device` などでコンテナへ公開してください。
例えば `/dev/px4card0` を使う場合の指定は `--device=/dev/px4card0:/dev/px4card0` です。
デバイス名は機種によって異なるため、ホスト上に作成されたカードデバイスの名前に合わせてください。
コンテナ内で pcscd を起動し、実行ユーザーが公開したデバイスを読み書きできることと、
pcscd が対象リーダーを認識することを確認してください。

Docker イメージ作成時はデバイスが接続されていないため、カードリーダーの認識確認は
デバイスを公開したコンテナの起動後が対象です。systemd を使わないコンテナでは、
`systemctl restart pcscd` の代わりに `pcscd -f` などで pcscd を起動できます。

## ソースからのビルド

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
```

### Debian パッケージの作成

Debian / Ubuntu では、ルートの `build_ifd_deb.sh` から `debhelper` を使って
IFD ハンドラー単体のバイナリパッケージを作成できます。

```bash
sudo apt-get install build-essential debhelper dpkg-dev libpcsclite-dev
./build_ifd_deb.sh
```

上の例はリポジトリのルートからの実行例です。スクリプトは任意の作業ディレクトリから
呼び出せて、引数で出力先ディレクトリを指定できます。

```bash
./build_ifd_deb.sh /tmp/px4-packages
```

既定の出力先はリポジトリの1つ上のディレクトリ（ルートから見て `../`）、ファイル名は
`px4-ifd_<版番号>_<アーキテクチャ>.deb` です。版番号は `update_version.sh` の
更新対象であるルートの `dkms.conf` から取得するため、パッケージ用の版番号管理は不要です。
ビルド対象は実行環境のアーキテクチャで、クロスビルドは対象外です。

ソースのコピー、コンパイル、梱包は一時ディレクトリ内で完結するため、
既存の `.o` / `.so` の削除や再利用は不要で、ビルドに root 権限も不要です。
`debhelper` によってビルド環境に応じた共有ライブラリ依存関係が付くため、
配布先の Debian / Ubuntu ごとのビルド環境を用意してください。

生成したパッケージは次のようにインストールできます。

```bash
sudo apt install ../px4-ifd_*.deb
```

パッケージには `libpx4ifd.so`、`Info.plist`、説明文書、ライセンス情報が含まれます。
`pcscd` は必須依存、`px4-drv-dkms` は関連パッケージの案内（`Suggests`）です。
カーネルドライバの手動導入や Docker コンテナ内での利用でも、カードデバイスノードを
提供する版の px4_drv がホスト側に必要です。
プラグインの配置先は `make install` と同じ `/usr/lib/pcsc/drivers/` 以下です。

導入時のカード通信の中断を避けるため、パッケージによる pcscd の自動再起動はありません。
既に pcscd が動いている場合は、カードを使用する録画などを終了した後で
`sudo systemctl restart pcscd` を実行してください。

GitHub Actions の `build.yml` では PR と手動実行を対象に、amd64 / arm64 の
ネイティブランナー上の Ubuntu 22.04 コンテナでパッケージがビルドされます。
ビルド後は lintian とインストール・削除の検証結果を確認できます。
タグの push を対象とする `build_and_release.yml` では、既存の DKMS パッケージと
IFD の両アーキテクチャの検証がすべて成功すると、3つの `.deb` が同じ Release に添付されます。
これらの検証はパッケージを対象とするため、実機でのカード通信と TS 同時受信の確認は別途必要です。

## 使い方

### 1. カーネルドライバのロード

Docker を利用する場合、この手順はホスト側で実行してください。

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
cat /usr/lib/pcsc/drivers/ifd-px4.bundle/Contents/Info.plist

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
         ↓ USB
   [IT930x ブリッジ]
         ↓ UART
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

## ライセンス

GPL v2（カーネルドライバと同じ）

## 参考情報

- PC/SC Workgroup: https://pcscworkgroup.com/
- ISO/IEC 7816-3: Smart card physical characteristics
- pcscd: https://pcsclite.apdu.fr/
