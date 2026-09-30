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

仕様により XIT-SQR100 は BS/CS アンテナへの電源供給に対応していません。そのため LNB 電圧制御 GPIO の TODO は対象外です。Ghidra だけでは基板の電源 GPIO、IT9303 の入力ポート・I²C バス、CXD2856ER のアドレス、TS の実際の同期形式を確定できず、これらは回路図または実機観測まで未確認です。初回の Linux モジュールビルドも、実行環境に `/lib/modules/25.6.0/build` がなく未確認です。

ソースレビューでは、ISDB-T 選局の3バイト書き込みで2バイト配列を使っていた範囲外アクセスを修正し、FREIA 参照実装に合わせてチップID判定マスク、入力周波数範囲、ISDB-S の安定待ちを反映しました。選局や停止が途中で失敗したときはチューナー状態を不明へ戻し、停止時の I²C エラーを呼び出し元へ返します。LNB 給電に対応しない仕様に合わせて、仮の GPIO11 制御と LNB power callback も除去しました。実機での ISDB-T/S 選局、連続 TS 受信、切替、基板 GPIO、ドロップ・TEI・continuity counter は未検証です。

### 2026-09-30 Ghidra GPIO / TS 設定追補

`FUN_140006684` (`DRV_InitCXD2856GPIO`) と `FUN_14000c65c` (`DRV_Initialize`) のレジスタ書き込みを [`driver/it930x.c`](../driver/it930x.c) の GPIO register table と照合すると、GPIO 番号、方向、出力値、順序を読み取れます。`FUN_14000c65c` の CXD2856+CXD6866 分岐では GPIOH2 を出力 Low にした後、100 ms 待って High にし、GPIOH5/7/1 を High、GPIOH14/6 を Low に設定します。GPIOH15/16 は入力設定です。

一方、`FUN_140006684` は GPIOH5 を出力 Low に設定します。`FUN_14000c65c` の CXD2856+CXD6866 分岐は同じ GPIOH5 を High にするうえ、GPIOH2 の遅延 High 遷移も異なります。したがって、Windows バイナリから GPIO レベルの候補は得られますが、基板で各 GPIO が担う信号、信号の論理極性、どの製品構成で各初期化経路が呼ばれるかを特定するまでは、どちらかの列を XIT-SQR100 へ転記できません。

`FUN_140055120` は `FUN_140055288` を経てレジスタ I/O を行い、`FUN_1400409e8` は `0x100` 以上のレジスタ番号を2-byteで USB ブリッジへ渡します。GPIO レジスタ番号は Linux 側の GPIO 表との照合で GPIOH番号に対応づけました。`FUN_140006c44` は NLC index 0/1 に対し tuner 側の `0xC0`/`0xC2` と demod 作成設定の `0xC8`/`0xCA` を使いますが、XIT-SQR100 にどの index 構成が使われるか、IT9303 の入力 port / I²C bus まではこの関数だけでは確定しません。

`FUN_14000c65c` の TS 同期設定は `param_1 + 0x311e0c` の実行時値で分岐します。値が1の場合は NLC 番号ごとの同期バイトを設定し、値が2の場合は4個の同期バイトを設定します。XIT-SQR100 の初期化時にどちらの値が入るかを、この3関数からは追えていないため、通常の `0x47` 同期と断定できません。

次に Windows ドライバーの PID から chip/NLC 設定、GPIO 初期化呼び出し、TS 同期モードの設定元へ至る経路を追います。この結果、レジスタ操作が製品構成に結び付いても、GPIO の基板上の接続先と信号極性には回路図または実測が必要です。

### 2026-10-01 Ghidra チューニング呼び出し経路追補

`FUN_1400eee90` はログ文字列 `OutputSetDeviceState` の通り、Kernel Streaming pin の STOP / ACQUIRE / PAUSE / RUN 状態遷移を扱います。ACQUIRE 時には `FUN_140020440` を呼び、周波数設定を進めます。STOP 時は実行中ハンドル数などを更新して受信スレッドを止める経路があります。

`FUN_140020440` は mutex `param_1 + 0x2eeef8` を取得して `FUN_14000d684` を呼び出す薄い排他ラッパーです。`FUN_14000d684` はデバイスの chip type による周波数設定関数の dispatch で、type `0x21` は `FUN_140013810` (`DRV_SetFreqBwSonyCXD2856ISDBT`) へ進みます。この関数は保存済みまたは新規の周波数・帯域幅を選び、周波数が 900 MHz 未満なら CXD2856 の ISDB-T Tune、900 MHz 以上なら ISDB-S Tune を呼び、TS lock を読みます。lock 成功後には100 ms待つ経路もあります。

この経路は `0x21` が CXD2856/CXD6866 構成の tuner 設定処理へ接続されることと、周波数設定・ロック確認の流れを示します。一方、これだけでは USB PID から `0x21` が選ばれる条件、前回の GPIO 初期化関数との接続、XIT-SQR100 基板上の GPIO 配線は確定しません。

`FUN_1400eb090` はログ上 `DeviceStart` として、登録済みの chip/filter 構成に応じた BDA filter factory を作ります。device の chip type が `!` (`0x21`) の場合、filter factory 登録前に `FUN_140020224` を呼びます。同関数は mutex 下で `FUN_140006684` を実行するため、前述の GPIO レジスタ列がこの CXD2856+CXD6866 構成の DeviceStart 経路で使われることを確認できました。

`FUN_1400f1fa0` は Windows の電源状態遷移を扱い、D0 への復帰ではデバイスを再初期化し、低電力側では各 filter の稼働状態を反映して selective suspend 等へ進みます。chip type `!` の場合、低電力側でも `FUN_140020224` 経由で `FUN_140006684` を呼ぶ経路があります。したがって GPIO 列は単発のチューナー設定だけでなく、DeviceStart と電源状態遷移の両方に結び付いています。どの USB PID がこの chip type を構成するか、および基板信号名と極性は別途確認が必要です。

### 2026-10-01 Ghidra B-CAS 経路と px4_drv の照合

列挙された `FUN_14000195c` / `FUN_140001aa8` / `FUN_140001a00` / `FUN_140001b84` / `FUN_140001d84` / `FUN_140001e60` / `FUN_140001c74` は、Windows ドライバーの B-CAS UART API ラッパーです。通常経路は IT930x の UART command と状態レジスタを使い、Linux の [`driver/it930x.c`](../driver/it930x.c) と下位通信はよく一致します。具体的には UART command `0x33` (read)、`0x34` (write)、`0x35` (baud rate)、`0x37` (mode)、ready register `0x496a`、受信長 `0x496b`、送信確定 register `0x4965` が一致します。関数名だけでなく本文を確認すると、`FUN_14000195c` はログに ResetUartCard と出しますが、実際には GPIO H6 を入力にしてカード検出を行います。また `FUN_140001a00` はログに SentUARTData と出す一方、実体は command `0x33` による受信データ読み出しで、送信は `FUN_140001b84` の command `0x34` です。通常カード検出は GPIO H6 を入力として読み Low Active とし、reset は GPIO H14 を Low にして UART reset register `0x7904` に `2` を書き、5 ms 後に High に戻します。Linux 側も同じ H6 / H14、register と pulse 時間を使います。Windows の検出ラッパーは H6 を都度入力設定しますが、Linux は初期化時に入力設定を済ませる違いがあります。

ATR と通信状態管理には違いがあります。Windows の `FUN_140001d84` は受信長が13 byte のときに ATR を読み出す経路で、reset 後の `FUN_140001aa8` は ready register を10 ms間隔でポーリングします。列挙された関数内では有限回数の上限が見当たりません。一方、WinUSB 版 [`smart_card.cpp`](../winusb/src/DriverHost_PX4/smart_card.cpp) は ATR を解析して期待長を決め、絶対期限で受信完了を待ち、挿抜や失敗時にセッションを破棄します。T=1 の block handling も WinUSB 版の上位層が持ち、列挙された Windows ドライバー関数は主に UART byte transport を公開しています。

baud rate API にも差があります。Windows の `FUN_140001c74` は 9600 / 19200 / 38400 をそれぞれ `0` / `1` / `2` へ変換しますが、その呼び出し先 `FUN_140052384` が受理するのは `0` / `1` のみです。reset 用の別 helper `FUN_14005481c` は `2` も受理します。Linux 側は 38400 に `0xef` を指定する特別扱いを実装しています。この差は 38400 baud の対応経路とカード種別条件を追加で調べる必要があります。

`FUN_140002794` / `FUN_140002838` はログ上 Extended B-CAS API で、通常経路とは異なる GPIO H15 (card detect) と H1 (reset) を使います。これは Linux の標準 H6 / H14 配線とは一致しない別 board variant と考えられますが、どの製品がこの経路を選ぶかは未特定です。`FUN_14000c65c` は設定 byte `param_1 + 0x311e11` が `2` の場合に B-CAS UART mode (`command 0x37`, value `1`) を初期化します。列挙中の `FUN_14000c65c` 重複は同一関数です。
