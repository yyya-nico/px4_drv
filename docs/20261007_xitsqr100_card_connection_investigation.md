# PIXELA XIT-SQR100 のカード接続エラー調査

検証環境は Windows、Visual Studio 2022 v143、PIXELA XIT-SQR100 1台です。内蔵カードは挿入中で、利用者の申告では受信・録画は停止中でした。カードの種類、前回試験からの向きの変更有無、アンテナ接続は未確認です。検証日は2026年10月7日です。

## 現象と失敗箇所

CardIDViewer（libaribb25 を使用するカードIDビューアー）で「ID取得」を実行すると、カード初期化失敗と PC/SC エラー `0x80100016` (`SCARD_E_NOT_TRANSACTED`) が表示されました。カード抜去時には未挿入として扱われるとの利用者報告がありました。

アプリに配置された `WinSCard.dll` と `DriverHost_PX4.exe` は、リポジトリの x64 `Release-static` 出力と SHA-256 が一致しました。ビューアーは `SCardBeginTransaction` / `SCardEndTransaction` を呼んでいません。

DLL を直接呼ぶ試験では `SCardConnectW` が3回とも `0x80100016` を返し、処理時間は235～252 msでした。libaribb25 の初期設定APDUを送信する前の失敗です。

診断用 DriverHost では、`ReadAtr` が33バイトすべて `00` のデータを読み出して `-EBADMSG` を返しました。拡張UARTの受信長レジスタ `0x4956` は、毎回 `A6 4F`（42,575）を返していました。カードを挿したままUSBを抜いて再接続した後も、同じ形式不正が再現しました。

この実装では `-EPROTO` / `-EBADMSG` が `SCARD_E_NOT_TRANSACTED` へ変換されるため、Windowsの定型文「存在しないトランザクションを終了しようとしました」が表示されます。今回確認できた失敗箇所はATR取得であり、トランザクション終了処理ではありません。

## 診断用ビルドの比較

製品コードとアプリの配置ファイルへの変更を避けるため、ソースとプロジェクトを別の診断用ディレクトリへ複製して比較しました。表の変更は診断用ビルドだけに適用したものです。GPIO H15 / H1 と機種固有の電源管理は維持しました。

| 条件 | 結果 |
|---|---|
| UART mode 5 を mode 2 に置換 | 同じ形式不正 |
| 通常UARTの受信長 `0x496b` を使用 | ATR先頭 `3B F0 12 00 FF` の後にタイムアウト |
| 通常UARTの受信完了通知 `0x496a` も使用 | タイムアウト |
| 上記に UART mode 1 を適用 | タイムアウト |
| 上記に UARTリセット `0x7904=2` を追加 | 正常な13バイトATRとT=1初期化を確認、接続3/3成功 |
| 現行の拡張UART経路にリセットだけ追加 | 同じ形式不正 |
| 通常受信長・完了通知とリセットを使用し、modeを現行の2 / 5へ戻す | 接続3/3成功、初期設定APDU6/6成功 |

成功時のATRは、[2026年10月6日の検証記録](20261006_xitsqr100_card_atr_validation.md)と一致しました。

```text
3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99
```

最後の成功構成では、次のAPDUを受信バッファ容量128バイト / 4,096バイトで比較しました。

| コマンド | 成功数 | 応答長 | 最大処理時間 | 応答ステータス |
|---|---|---|---|---|
| 初期設定 `90 30 00 00 00` | 6/6 | 61バイト | 93 ms | ステータス値の個別記録なし |
| カードID取得 `90 32 00 00 00` | 6/6 | 19バイト | 52 ms | 6回とも `9000` |

カードIDそのものと初期設定応答の内容は、調査結果の判定に不要なため記録対象外です。カードID取得はDLLへの直接呼び出しで確認したもので、ビューアー画面の表示まで検証した結果ではありません。

## 試験方法

診断用PowerShellから、アプリに配置されたDLLの `SCardEstablishContext`、`SCardListReadersW`、`SCardConnectW`、`SCardTransmit`、`SCardDisconnect`、`SCardReleaseContext` をP/Invokeで呼び出しました。対象名は `XIT-SQR100` に限定し、共有モード、T=1で接続しました。切断は `SCARD_LEAVE_CARD` です。

`connect_diag.ps1` は3回の接続と切断だけ、`card_diag.ps1` は各接続で初期設定APDUを2回、`id_diag.ps1` は各接続でカードID取得APDUを2回実行する診断用スクリプトです。診断用DriverHostは、既存ホストが存在しないことを確認してから起動しました。

診断用プロジェクトのビルド指定は次のとおりです。出力と中間生成物は診断用ディレクトリに配置しました。

```powershell
MSBuild DriverHost_PX4.vcxproj /p:Configuration=Release-static /p:Platform=x64 /p:PlatformTarget=x64 /p:SolutionDir=<リポジトリのwinusbディレクトリ>/ /p:OutDir=<診断用ディレクトリ>/ /p:IntDir=<診断用ディレクトリ>/obj/ /v:minimal /nologo
powershell -NoProfile -File connect_diag.ps1
powershell -NoProfile -File card_diag.ps1
powershell -NoProfile -File id_diag.ps1
```

診断用ソース、スクリプト、バイナリ、ログはリポジトリ外の調査用領域に保存されており、この文書への追加には含まれていません。

既存ビルドの `winusb/build/x64/Release-static/smart_card_state_test.exe` と `winusb/build/x86/Release-static/smart_card_state_test.exe` は、どちらも `passed` でした。これは既存の状態遷移試験の結果であり、診断用UART変更の回帰検証ではありません。

## 判断と未確認条件

この個体では、通常UARTの受信長・完了通知とUARTリセットを組み合わせると、接続とAPDU通信が回復することを確認できました。UART modeの変更は成功に必要ではありませんでした。ただし、拡張受信長が不正になる理由と、2026年10月6日に現行構成で成功した条件との差は未確定です。

通常UARTの受信レジスタで回復したことは、GPIO配線まで通常機種と同じである根拠にはなりません。製品修正ではGPIO H15 / H1と共有電源の契約を維持し、UART経路の選択を機種ごとに検証する必要があります。この調査では製品コードの修正と配布物の更新は対象外です。

実機試験はx64、短時間の比較に限定されています。長時間通信、成功構成でのUSB再接続、カード挿抜、受信同時負荷、D/E/S、他機種・ロット、他カード、外付けリーダーは未確認です。受信停止中のAPDU成功から、TS受信への影響は判断できません。

調査後の製品コードへの反映とx86 / x64の検証結果は、[UART修正と検証記録](20261008_xitsqr100_card_uart_fix.md)に記載しています。この文書の「現行ビルド」は調査時点の未修正ビルドを指します。
