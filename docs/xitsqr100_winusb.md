# XIT-SQR100 の WinUSB 対応

対象は PIXELA XIT-SQR100（USB VID `06b8`、PID `106b`）です。Windows の実機試験前の実験的実装で、受信回路の設定は Linux 版の CXD2856ER / CXD6866AER 処理と共有します。

## 構成と維持する契約

WinUSB 版に機種実装が存在しなかったため、専用 `XitSqr100Device` と1基の地デジ・BS・CS 共用受信機を追加します。製品固有の GPIO、電源、I2C、通常 `0x47` TS はこの機種へ局所化します。カード初期化はカード接続時に行うため、カード初期化の失敗は受信機の登録を中止させません。カードと受信機の双方が電源利用を保持し、最後の利用者が閉じたときにだけ受信回路の電源を停止する構成です。カード通信には既存の SmartCard / WinSCard_PX4 を使用し、ATR、T=1、公開 WinSCard API、名前付きパイプの形式は既存の契約を維持します。

カードの拡張 UART は通常機種の GPIO H6 / H14 と異なります。保存済み XIT-SQR100 USB キャプチャでは H15 の入力読み出し、H7 の出力 Low、H1 の Low / High、UART mode `2` / `5`、受信長 `0x4956` の big-endian 2-byte 読み出し、送信前 `0x4953=0` と `0x4965=1` を確認できます。共通 IT930x 層には拡張経路専用の初期化入口を追加し、通常の初期化入口は従来の GPIO とレジスタを選びます。

キャプチャには暗号鍵の設定と暗号化された UART データも含まれます。鍵設定を省いた mode `2` / `5` で平文の ATR / T=1 が返るかは未確認です。GPIO の電源信号としての意味、カードリセット後の受信 FIFO の破棄、待機時間、別ロットの差も実機確認が必要です。形式不正を成功として扱う処理はなく、既存 ATR / T=1 検証で失敗を返す構成です。

## 導入とカタログ更新

`winusb/pkg/inf/XIT-SQR100.inf` と DriverHost の機種定義は同じ DeviceInterfaceGUID を使用します。配布用 BonDriver は `BonDriver_XIT-SQR100.dll` / `.ini` で、受信機は1基です。BS/CS アンテナ給電機能がないため、設定の `LNBPower=0` が必要です。

新規 INF は CRLF・BOM なしです。既存 CAT は新規 INF を含まないため、そのままでは署名検証と配布ビルドを通過できません。Windows 環境で以下を実行して CAT を再生成・検証してから `build.ps1` を実行してください。

```powershell
Set-Location winusb
.\pkg\signing-tools\sign.ps1
.\pkg\signing-tools\verify.ps1 -DriverPath .\pkg\inf
.\build.ps1
```

WinUSB の導入により、この USB デバイスをメーカー製 Windows ドライバで使うにはドライバの戻しが必要になります。

## 検証範囲

自動試験の対象は拡張 / 通常 UART のレジスタ選択、2-byte 受信長、短い応答、I/O エラー、TS の分割入力・同期回復・選局時の端数破棄です。共通カード状態試験と共通 TS 同期試験も回帰確認に含めます。

Windows x86 / x64 の VS2022 v143 ビルド、実機の ATR / APDU、地デジ・BS・CS 受信、選局反復、受信と APDU の同時負荷、カード挿抜、USB 抜去、終了、長時間受信は別途必要です。対象カードの配置、アンテナ接続、時間、D/E/S、同期・TEI・continuity error、空受信、APDU 失敗数、最大処理時間を試験記録へ残してください。Linux 版の既存受信結果は Windows 版の検証の代用にはなりません。

## この変更で実行した確認

検証環境は Linux x86_64、gcc / g++ 13、カーネルヘッダー `7.0.0-34-generic`、一時領域に展開した MinGW-w64 GCC 13.2 です。実機を操作する試験は含まれないため、カード配置・アンテナ接続・受信時間・D/E/S・APDU の実測値はありません。

- `cc -std=c11 -Wall -Wextra -fsanitize=address,undefined driver/tests/it930x_card_transport_test.c -o /tmp/it930x_card_transport_test` と生成した試験の実行: 成功。通常 / 拡張 GPIO、300-byte 受信長、255-byte 出力容量、短い受信長 / UART 応答、初期化エラーを確認。
- `g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Idriver -Iwinusb/src/DriverHost_PX4 winusb/tests/xit_ts_framer_test.cpp -o /tmp/xit_ts_framer_test` と生成した試験の実行: 成功。
- 共通 `ts_sync_condition_test.cpp`: Linux 上の g++ によるビルド・実行成功。
- 共通 `smart_card_state_test.cpp` と `smart_card.cpp`: OS 非依存の模擬 I/O を Linux 上で実行して成功。試験用の `it930x.h` は UART baud enum と errno だけを提供する代替ヘッダーであり、実 USB 通信は対象外。
- `make -C <一時コピーの driver> KDIR=/usr/src/linux-headers-7.0.0-34-generic`: モジュールビルド成功。共有 `include/ptx_ioctl.h` も同じ相対配置へコピー。ロード・再接続は対象外。
- MinGW の `x86_64-w64-mingw32` / `i686-w64-mingw32` の `g++-posix -std=c++17` / `gcc-posix -std=c11`、include paths は `driver`、`winusb/src/common`、`winusb/src/DriverHost_PX4`: 新機種実装、`device_manager.cpp`、共有 `it930x.c` / `cxd6866.c` / `cxd2856er.c` のオブジェクト生成と TS 試験実行ファイル生成が成功。Visual Studio のソリューションビルドと生成 EXE の Windows 実行は未確認。
- INF の CRLF・BOM なし、INF と機種定義の GUID 一致、プロジェクト XML、`git diff --check`: 成功。CAT の再生成と Windows による収録・署名検証は利用者側で実施予定。
