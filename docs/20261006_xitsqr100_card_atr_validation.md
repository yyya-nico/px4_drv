# XIT-SQR100 の Windows カード ATR 検証

検証環境は Windows、Visual Studio 2022 v143、USB 接続の PIXELA XIT-SQR100 1台です。内蔵カードは利用者が挿入・抜去・再挿入し、地デジと BS/CS のアンテナは利用者の申告では接続済みです。外付けカードリーダーは列挙されていません。検証日は2026年10月6日です。

## 原因と修正

修正前はカード挿入中の `SCardConnectW` が3回とも `SCARD_E_NO_SMARTCARD` (`0x8010000C`) を返し、ATR 取得へ進めませんでした。XIT-SQR100 の H15 読み出しを反転していたため、High の挿入状態が未挿入へ変換されていました。

反転を除いたビルドでは、同じカードから3回とも ATR を取得できました。カードを抜くと3回とも未挿入を返し、再挿入後は3回とも同じ ATR を取得できたため、H15 の挿入時 High を実機で確認できました。通常機種の検出経路には変更がありません。

API 試験で `SCARD_ATTR_VENDOR_NAME` が `ERROR_NOT_SUPPORTED` を返したため、既存の表示名プレフィックス判定へ `PIXELA ` を追加しました。

## ATR とカード通信

診断用 PowerShell から検証用 DLL の `SCardEstablishContext`、`SCardListReadersW`、`SCardConnectW`、`SCardState` を呼び出しました。接続は共有モード、許可プロトコルは T=0 / T=1、切断は `SCARD_LEAVE_CARD` です。初回の接続処理は380～414 ms、再挿入後は389～408 msでした。

```text
ATR: 3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99
ATR length: 13
Protocol: SCARD_PROTOCOL_T1
State: SCARD_SPECIFIC
```

`card_reader_reliability.exe 30 200 XIT-SQR100` の30秒試験は、APDU 成功112回、失敗0回、200 ms以上の応答0回、最大処理時間66 msでした。

## ビルドと API 試験

作業ディレクトリはリポジトリルートです。以下のソリューションビルドが x86 / x64 とも成功しました。

```powershell
MSBuild winusb/px4_winusb.sln /p:Configuration=Release-static /p:Platform=x64 /p:PlatformToolset=v143
MSBuild winusb/px4_winusb.sln /p:Configuration=Release-static /p:Platform=x86 /p:PlatformToolset=v143
```

`winscard_api_test.vcxproj` は `Configuration=Release` と、x64 では `Platform=x64;PlatformTarget=x64`、x86 では `Platform=Win32;PlatformTarget=x86` でビルドしました。試験 EXE と同じ検証用ディレクトリに対応する `WinSCard.dll`、DriverHost、設定、ファームウェアを配置しています。x64 / x86 の API 試験はカード挿入中に成功し、ATR、属性、共有・排他、トランザクション、APDU を検証しました。x86 API 試験は x64 DriverHost との接続も含みます。

同時負荷中に実行した最初の x86 API 試験は、他のカード接続が残るため排他への再接続に失敗しました。同時負荷終了後の単独試験で成功しており、この失敗を検出極性の不具合とは扱っていません。

その後、既存の検証用ホストがアイドル終了した状態で32bit PowerShellから x86 DLL を呼び出し、x86 DriverHost の起動を確認しました。この組み合わせでも ATR は3回とも一致し、接続処理は400～416 msでした。

`smart_card_state_test.exe`、`xit_ts_framer_test.exe`、`ts_sync_condition_test.exe`、`ringbuffer_purge_test.exe` は x86 / x64 とも成功しました。`channel_purge_test.exe` は実機用引数が必要で、引数なしの呼び出しは終了コード2だったため成功試験に含めていません。TS バッファやチャンネル切り替え処理は今回の変更対象外です。`git diff --check` も成功しました。

## TS 同時負荷の制限

既存の `card_stream_stress.cpp` を v143 x64、C++17、`winusb/include`、`Winscard.lib` でビルドし、検証用 BonDriver と同じディレクトリに配置しました。地デジは space 0 / channel 0（13Ch）、BS は space 1 / channel 0（BS01/TS0）、CS は space 2 / channel 0 を指定しました。

地デジは受信のみ10秒、カード同時負荷30秒で APDU 成功346回、失敗0回、信号値31.13でした。BS は受信のみ5秒、同時負荷10秒で APDU 成功116回、失敗0回、信号値18.50でした。しかし両試験とも TS は0パケットで、同期・TEI・continuity の0件は受信品質を示す値ではありません。ツールの終了コード0だけでは受信試験の成功と判断できないため、TS への影響は未確認です。

CS も受信のみ5秒、同時負荷10秒で TS は0パケットでした。APDU 成功115回、失敗0回、信号値16.60でした。既存ツールの adaptation field 計数には長さの追加検証が必要ですが、今回はパケット自体が届いていないため、この計数を受信品質の根拠には使っていません。

長時間録画、USB 抜去、他の製品・ロット、外付けカードリーダー、復号後の D/E/S は未確認です。今回の修正範囲は Windows の XIT-SQR100 カード検出とメーカー属性に限定されています。
