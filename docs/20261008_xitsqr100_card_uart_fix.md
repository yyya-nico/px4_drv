# PIXELA XIT-SQR100 のカードUART受信修正

検証環境は Windows、Visual Studio 2022 v143、PIXELA XIT-SQR100 1台です。内蔵カードは挿入中で、試験開始前の受信・録画は停止中でした。利用者の申告では初回試験時にアンテナは未接続で、受信処理の簡素化後の再試験時に接続済みです。カードの種類、他カードとの比較は未確認です。作業は2026年10月7日から8日に実施しました。

## 修正内容

[カード接続エラー調査](20261007_xitsqr100_card_connection_investigation.md)では、拡張UARTの受信長 `0x4956` が `A6 4F` を返し、ATR取得が形式不正で失敗しました。受信長 `0x496b` と受信完了通知 `0x496a` を使う処理へ統一しました。`it930x_bcas_get_data` は1バイトの `available` に `it930x_read_reg` で直接受信長を読みます。途中の受信長から完了を判断する処理は不要です。

UARTリセットは、作業開始時にすでに存在した共通化の差分を維持しています。カードのGPIOをLowにした後、`0x7904=2`、9600 baud設定、既存の5 ms待機、GPIO Highの順に処理します。UART初期化が失敗すると、カードをHighへ戻す前にエラーが上位へ返ります。

XIT-SQR100 のH15検出、H7初期状態、H1リセット、mode 2 / 5、送信前 `0x4953=0`、電源管理には今回の受信経路変更による差分はありません。製品コードで拡張受信を使う他の呼び出しがなかったため、最初の修正で追加した `bcas_normal_rx` と `it930x_bcas_rx_length`、`0x4956` の受信処理は削除しました。`bcas_extended` はGPIOと送信前処理の選択に必要なため残っています。公開APIとIPC形式への変更はありません。

## ビルドと自動試験

リポジトリルートから次のコマンドでソリューション全体をビルドし、x64 / x86とも成功しました。

```powershell
MSBuild winusb/px4_winusb.sln /p:Configuration=Release-static /p:Platform=x64 /p:PlatformToolset=v143 /v:minimal /nologo
MSBuild winusb/px4_winusb.sln /p:Configuration=Release-static /p:Platform=x86 /p:PlatformToolset=v143 /v:minimal /nologo
```

`driver/tests/it930x_card_transport_test.c` はv143のx64 / x86開発環境で `/std:c11 /utf-8 /W3` を指定してコンパイルし、両アーキテクチャで成功しました。生成物はリポジトリ外の検証用ディレクトリに配置しました。

```powershell
cl /nologo /utf-8 /std:c11 /W3 driver/tests/it930x_card_transport_test.c /Fo<検証用ディレクトリ>/it930x_card_transport_test.obj /Fe<検証用ディレクトリ>/it930x_card_transport_test.exe
```

模擬USB試験では、1バイト受信長の上限255バイト、呼び出し側の容量を超えないこと、短いUART応答、通常機種のGPIOとUART、XIT-SQR100 の受信途中と完了通知の区別、UARTリセット、UARTリセット失敗後にGPIOがLowを維持すること、受信I/Oエラーの伝播を確認しました。テストの模擬受信長を1バイトへ格納する既存箇所でC4244警告がありました。

`winusb/build/<architecture>/Release-static/` の `smart_card_state_test.exe`、`ts_sync_condition_test.exe`、`xit_ts_framer_test.exe`、`ringbuffer_purge_test.exe` は、x64 / x86とも成功しました。`git diff --check` も成功しました。Linuxカーネル環境は利用できなかったため、カーネルモジュールのビルドは未確認です。

## 実機試験

更新したDriverHost、WinSCard.dll、設定、ファームウェアと試験EXEを、アーキテクチャ別の検証用ディレクトリへ配置しました。既存ホストが存在しないことを確認してから実行しています。ビューアーの配置ファイルの差し替えは今回の対象外です。

PowerShellの診断スクリプトは検証用DLLを直接P/Invokeし、共有モード、T=1で3回接続、各接続で受信容量128 / 4,096バイトのAPDUを各1回送信しました。切断は `SCARD_LEAVE_CARD` です。x86は32bit PowerShellから実行しました。

| 試験 | x64 | x86 |
|---|---|---|
| 初期設定時の接続 | 3/3成功、419～430 ms | 3/3成功、418～430 ms |
| 初期設定APDU `90 30 00 00 00` | 6/6成功、61バイト、最大80 ms | 6/6成功、61バイト、最大82 ms |
| `winscard_api_test.exe` | passed、終了コード0 | passed、終了コード0 |
| ID取得APDU `90 32 00 00 00` | 6/6成功、19バイト、最大48 ms | 6/6成功、19バイト、最大47 ms |

ID取得の応答末尾は全回 `9000` でした。カードID自体と初期設定応答の内容は、判定に不要なため記録対象外です。API試験では接続、ATR、属性、共有・排他、トランザクション、APDUを検証しました。外付けリーダーは列挙されず、試験対象外でした。

次のx64の30秒試験は、成功108回、失敗0回、200 ms以上の応答0回、最大86 ms、終了コード0でした。

```powershell
card_reader_reliability.exe 30 200 XIT-SQR100
```

## 受信試験と残る条件

次の地デジ同時負荷試験を試みましたが、最初は選局失敗（終了コード6）、作業ディレクトリを検証用ディレクトリにした再試行ではチューナー開始失敗（終了コード5）でした。利用者からアンテナ未接続の申告があったため、TS受信やカード通信の同時負荷結果として扱える計測は得られていません。今回の修正がこれらの失敗を起こしたかは未確定です。

```powershell
card_stream_stress.exe BonDriver_XIT-SQR100.dll 0 0 5 30 - XIT-SQR100
```

この段階では長時間通信、修正ビルドでのUSB抜去・再接続、カード挿抜、BS / CSを含むTS同時負荷、TVTestのD/E/S、他機種・ロット、他カード、外付けリーダーは未確認でした。配布物の再生成とカタログ検証を含む `build.ps1` 全体は、今回のソース修正と実機確認の対象外です。

## 受信処理の簡素化後とアンテナ接続後の確認

`bcas_normal_rx` と `it930x_bcas_rx_length` を削除した版では、x64 / x86の模擬USB試験が成功しました。直接P/InvokeでのID取得も両アーキテクチャで6/6成功し、応答19バイト、全回 `9000`、最大処理時間はx64で57 ms、x86で58 msでした。

ソリューションの最初の再ビルドでは、両アーキテクチャの `smart_card_state_test` のリンクが `LNK1101: MSPDB140.DLL` のバージョン不一致で失敗しました。VS2022 の `VsDevCmd.bat` で開発環境を読み込んでから同じMSBuildを再実行したところ、x64 / x86のソリューション全体は成功しました。再生成した両アーキテクチャのカード状態遷移・TS同期条件・XIT TSフレーマー試験も成功しました。DriverHost本体は最初の再ビルドでも両アーキテクチャで生成されており、以下の実機試験にはその簡素化済みのx64出力を使用しました。

アンテナ接続後、x64で次の同時負荷試験を実行しました。

```powershell
card_stream_stress.exe BonDriver_XIT-SQR100.dll 0 0 5 30 - XIT-SQR100
card_stream_stress.exe BonDriver_XIT-SQR100.dll 1 0 3 10 - XIT-SQR100
card_stream_stress.exe BonDriver_XIT-SQR100.dll 2 0 3 10 - XIT-SQR100
```

| 対象・区間 | 秒数 | TSパケット数 | 同期エラー | TEI | 連続性エラー | APDU成功 / 失敗 |
|---|---:|---:|---:|---:|---:|---|
| 地デジ・受信のみ | 5 | 99,328 | 0 | 0 | 0 | 対象外 |
| 地デジ・カード同時負荷 | 30 | 598,016 | 0 | 0 | 0 | 337 / 0 |
| BS・受信のみ | 3 | 34,816 | 0 | 0 | 0 | 対象外 |
| BS・カード同時負荷 | 10 | 115,712 | 0 | 0 | 0 | 112 / 0 |
| CS・受信のみ | 3 | 108,544 | 0 | 0 | 24 | 対象外 |
| CS・カード同時負荷 | 10 | 347,136 | 0 | 0 | 0 | 112 / 0 |

終了コードは全試験0で、信号値は地デジ32.47、BS18.50、CS16.70でした。ただしツールの終了条件は同時負荷区間の計数を使うため、CSの受信のみ区間で検出した連続性エラー24件を終了コード0から正常と判断することはできません。今回のカード修正との因果関係は未確定です。

短時間の同時負荷でカードAPDUとTSの双方が動作することは確認できました。長時間録画、修正ビルドでのUSB抜去・再接続とカード挿抜、異なるチャンネルへの切り替え、x86の実機受信、TVTestのD/E/S、他機種・ロット・カード、外付けリーダーは引き続き未確認です。
