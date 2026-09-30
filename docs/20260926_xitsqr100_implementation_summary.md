# XIT-SQR100 対応の実装状況 (2026-09-27 時点)

この文書は、GitHub Copilot との相談後に作成された実装サマリーを、現在のリポジトリと照合した記録です。XIT-SQR100 対応は実験的な段階にあり、ソースへ実装された部分と、機器固有の根拠または試験が不足している部分を分けて記載します。

## リポジトリで確認できる変更

以下のファイルに XIT-SQR100 用の変更が含まれています。

- `driver/px4_usb.h`、`driver/px4_usb.c`: USB VID/PID とデバイス種別、probe/disconnect、chrdev 登録
- `driver/xit_sqr100_device.h`、`driver/xit_sqr100_device.c`: Linux デバイス処理、TS 同期・受信、ISDB-T/S の制御経路
- `driver/cxd6866.h`、`driver/cxd6866.c`: CXD6866AER 用の tuner API と未実装スタブ
- `driver/Kbuild`: 新しいオブジェクトとデバイス数設定
- `etc/99-px4video.rules`: udev ルール
- `README.md`: 実験的対応機種としての掲載

## 実装済みの範囲

### USB デバイス登録

VID `0x06b8`、PID `0x106b` を XIT-SQR100 として扱う経路が `px4_usb.c` にあります。デバイス種別、切断処理、chrdev コンテキストの登録と解除も追加されています。

### デバイス処理の骨組み

`xit_sqr100_device.c` には電源制御、open/close、tune、lock 判定、ISDB-S stream ID と LNB 電圧、TS capture、CNR 取得の経路があります。TS 同期には通常の `0x47` を前提とする `px4_ts_has_plain_sync()` を使用しています。

ただし、これらの処理に設定されている機器固有の値が実機と一致することや、チューナーが実際に選局できることは確認されていません。コードが存在することを、機能検証済みとは扱いません。

### CXD6866AER tuner API

`cxd6866.c` は I²C ヘルパーと API の形を持ちますが、初期化と ISDB-T/S 選局シーケンスは未実装です。`cxd6866_init()`、`cxd6866_set_params_t()`、`cxd6866_set_params_s()` は `-EOPNOTSUPP` を返し、tuner は利用できません。したがって XIT-SQR100 の実受信対応は完成していません。

## FREIA 参照実装との照合

`sony_cxd_family/family_source/tuner/terr_cable_sat_freia/` に CXD6866AER (FREIA) を含む Sony の tuner 実装がありました。`refcode/sony_freia.c` はレジスタ I/O と選局手順、`sony_tuner_freia.c` は Sony 共通 tuner API からの呼び出しと待機を実装しています。このソースは CXD6866AER の動作手順を調べる一次資料として利用できます。ただし、同じチップを使うことだけでは XIT-SQR100 の接続や基板設定まで一致するとは限りません。

ソースから確認できる事項は次のとおりです。

- 初期化時に I²C レジスタ `0x7F` を読み、`data & 0xFC == 0xF8` を CXD6866AER として識別します。`sony_freia.h` は I²C アドレスを8-bit形式と定義し、既定値を `0xC0` としています。Linux の7-bit表記へ換算すると `0x60` です。
- 地上波選局は規格・帯域に応じた FREIA 設定で `TER_tune` を実行し、50 ms 待ってから `TER_tune_end` を呼びます。ISDB-T 6 MHz の表には既定 IF 3.55 MHz が記載されています。
- ISDB-S のシンボルレートは `28860 ksps` に固定されます。衛星選局は `SAT_tune` の後に10 ms待って `SAT_tune_end` を行い、上位 wrapper はさらに50 ms待ちます。
- 内部水晶を使う参照設定では `xosc_sel=0x04`、`xosc_cap_set=0x1E` を設定し、コメントは6 pF水晶向けと説明します。これは XIT-SQR100 の水晶周波数や実装部品を示す情報ではありません。

この照合により、`driver/xit_sqr100_device.c` の tuner I²C 7-bit address `0x60` は参照実装の既定アドレスと整合します。ただし、XIT-SQR100 で実際に応答すること、衛星モードで同じアドレスを使うこと、参照実装の水晶・電源・LNA 設定を採用できることは未確認です。Windows ドライバーの Ghidra 解析で確認した GPIO 初期化ログや CXD2856 経由の tuner I²C 操作も、この基板の具体的な GPIO 極性・選局レジスタ列を確定する証拠にはなりません。

### Windows ドライバーの tuner 関数

Ghidra でログ文字列の参照先を確認し、次の関数を特定しました。

| 関数 | 確認できた処理 |
|---|---|
| `FUN_140006c44` (`DRV_InitTuner`) | chip type `0x21` の場合に FREIA tuner と CXD2856 demod を作成・初期化し、TS 出力設定を適用 |
| `FUN_140019f08` (`DRV_WriteTunerReg`) | repeater を有効化して tuner register を書き、repeater を閉じた後、再度開いて register `0x80` に `1` を書く経路を実行 |
| `FUN_14000e224` (`DRV_ReadTunerReg`) | repeater を有効化して tuner register を読み、処理後に repeater を閉じる |

`DRV_InitTuner` は NLC index 0 で tuner address `0xC0`、index 1 で `0xC2` を FREIA 作成関数へ渡しています。各 index で demod 作成へ渡す値もそれぞれ `0xC8` と `0xCA` です。FREIA ソースのアドレス形式定義に照らすと tuner address は7-bit `0x60` / `0x61` に対応します。これは Windows ドライバーが複数 index 用に持つ設定で、XIT-SQR100 の単一 tuner にどちらが割り当たるかまでは特定できません。

I²C repeater の有効化・無効化は CXD2856 の API を介し、実際の tuner read/write は FREIA の callback へ委譲されています。書き込み後の register `0x80` への追加書き込みの目的は、逆コンパイル結果だけからは断定できません。これらの経路は通信構造の確認に使えますが、ログや関数名だけから GPIO の基板配線や特定チャンネルの選局シーケンスを復元したものではありません。

## 未確認の機器固有値

次の値は既存機種のコードを参考に置かれた候補値であり、XIT-SQR100 の回路図、データシート、または実機で確定していません。

| 項目 | 現在の候補 | 確認状況 |
|---|---:|---|
| CXD6866AER I²C 7-bit address | `0x60` | FREIA 参照実装の既定値と一致。XIT-SQR100 上の応答は未確認 |
| CXD6866AER crystal | 16 MHz | 未確認 |
| CXD6866AER LNA 設定 | 地上波・衛星とも有効 | 未確認 |
| IT9303 GPIO 電源シーケンス | GPIO 3/2 | 未確認 |
| LNB 電圧制御 GPIO | GPIO 11 | 未確認 |
| IT9303 I²C bus / input port | bus 3 / port 4 | 未確認 |
| CXD2856ER SLVT/SLVX address | `0x18` / `0x1a` | 未確認 |
| TS 同期形式 | 通常の `0x47` | 実機 TS 未確認 |

前回の `IT9300BDA.sys` 解析で見つかった `0xC0` / `0xC2` は、CXD2856 の tuner 経由 API に渡される8-bit形式のアドレス候補です。7-bit表記では `0x60` / `0x61` に相当します。ただし、Windows ドライバ内の複数デバイス用コードに現れた値であり、XIT-SQR100 の実装値だとは確認できていません。

同ドライバには `DRV_InitCXD2856GPIO` および `Set CXD2856+CXD6866 GPIO` の文字列もあります。文字列だけでは GPIO 番号、極性、初期化順序、XIT-SQR100 基板との対応は分かりません。既存の Linux コードへそのまま移す根拠にはなりません。

## 検証状況

この記録の作成時点で確認できているのはソース上の実装範囲と、Ghidra 上での Windows ドライバの文字列・シンボル情報です。以下は未確認です。

- Linux カーネルモジュールのビルド
- 実機での USB 認識、デバイス初期化、I²C 応答
- ISDB-T / ISDB-S の選局と TS 同期
- D/E/S、TEI、continuity counter、空受信の計数
- チャンネル切り替え時の同期回復
- LNB 電圧制御
- 長時間連続受信
- Windows WinUSB 版

実機試験を行う場合は、対象機種・アンテナ・カード配置・試験時間と、D/E/S、同期エラー、TEI、continuity counter、空受信時間を記録してください。実機を使えない条件では、未確認のまま残します。

## 次に必要な資料と作業

1. XIT-SQR100 の回路図または技術資料を入手し、IT9303 GPIO、I²C bus、CXD2856ER と CXD6866AER の接続を確認する。
2. FREIA 参照実装と Windows ドライバー解析を対応づけ、XIT-SQR100 の接続条件を確認してから初期化・選局・停止シーケンスを実装する。
3. CXD2856ER と tuner の I²C リピータ経路、アドレス形式、電源投入順を確認する。
4. Linux ビルドを行い、利用可能な自動試験を実行する。
5. 実機で ISDB-T/S の受信、チャンネル切り替え、LNB 制御、継続受信を検証する。

## 2026-09-27 時点の位置づけ

XIT-SQR100 の Linux 用デバイス登録と処理の骨組みはリポジトリにあります。CXD6866AER の選局処理は未実装で、GPIO・I²C・TS 形式も未確認です。README にある「実験的」対応の範囲を超えて、実受信対応済みとは判断できません。

## 2026-09-30 追補

上記は 2026-09-27 時点の記録です。その後、`driver/cxd6866.c` に Sony FREIA 参照実装を基にした初期化、ISDB-T/ISDB-S 選局、停止シーケンスが追加されました。Ghidra で `IT9300BDA.sys` の `DRV_InitTuner` (`FUN_140006c44`) を確認すると、NLC index 0 の FREIA 作成処理へ I²C アドレス `0xC0` と構成フラグ `0x10004000` が渡されます。参照実装の8-bitアドレスを Linux の7-bit表記にすると、実装中の `0x60` と一致します。

同バイナリの FREIA 初期化関数 (`FUN_14009f330`) は、レジスタ `0x7F` の値を `& 0xFC` で判定し、`0xF8` を CXD6866AER として扱います。FREIA 作成関数 (`FUN_14009f204`) は、拡張クロック入力が無効な場合に `xosc_sel=0x04`、`xosc_cap_set=0x30` を設定します。構成フラグ `0x10004000` では拡張クロック入力は有効にならないため、この2値は現行の `X_pon` 実装と一致します。ISDB-T/S の tuner API (`FUN_1400a1ae4`、`FUN_1400a0a74`) も、Sony 参照ソースと同じ入力周波数範囲、選局処理後の待機・完了処理を使います。Sony 参照ソースに基づくレジスタ列と、Windows ドライバーから読める構成値は、ソース上で整合することを確認しました。

メーカー仕様では XIT-SQR100 は BS/CS アンテナへの電源供給に対応していません ([製品仕様](https://download.pixela.co.jp/products/xit/sqr100/spec.html))。そのため LNB 電圧制御 GPIO の TODO は対象外です。Ghidra だけでは基板の電源 GPIO、IT9303 の入力ポート・I²C バス、CXD2856ER のアドレス、TS の実際の同期形式を確定できず、これらは回路図または実機観測まで未確認です。初回の Linux モジュールビルドも、実行環境に `/lib/modules/25.6.0/build` がなく未確認です。

ソースレビューでは、ISDB-T 選局の3バイト書き込みで2バイト配列を使っていた範囲外アクセスを修正し、FREIA 参照実装に合わせてチップID判定マスク、入力周波数範囲、ISDB-S の安定待ちを反映しました。選局や停止が途中で失敗したときはチューナー状態を不明へ戻し、停止時の I²C エラーを呼び出し元へ返します。LNB 給電に対応しない仕様に合わせて、仮の GPIO11 制御と LNB power callback も除去しました。実機での ISDB-T/S 選局、連続 TS 受信、切替、基板 GPIO、ドロップ・TEI・continuity counter は未検証です。
