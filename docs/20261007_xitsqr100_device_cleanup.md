# XitSqr100Device の重複整理と検証

検証環境は Windows、Visual Studio 2022 v143、PIXELA XIT-SQR100 1台です。実機試験はx64 BonDriverとx64 DriverHost、内蔵カード挿入中で実施しました。利用者の申告では試験前の受信・録画は停止中でした。アンテナ接続の申告は今回取得していませんが、地デジ・BS・CSでTSを取得できました。検証日は2026年10月7日です。

## 整理した重複

- `Receiver::open_` と `XitSqr100Device::receiver_open_` は同じロック下で同時に更新されていたため、開閉状態を後者へ統一しました。受信機の終了待機とカードの共有電源判定もこの状態を参照します。
- コンストラクターが受信機1基・index 0を検証するため、受信機登録のループを先頭要素の登録へ置き換えました。ゼロ初期化済みの `ReceiverInfo::data_id` の再代入も不要でした。
- `CloseCard` は冒頭で未接続状態を除外するため、後続条件に重複していた `card_open_` の確認を除きました。
- PXMLTと同一の衛星CNR表を `winusb/src/DriverHost_PX4/cxd2856er_cn_table.hpp` へまとめました。表の全要素が変更前と一致することを確認し、各機種の検索方法と境界での値選択は維持しました。

変更範囲は上記の状態・条件・定数の重複です。ハードウェア操作の順序、待機時間、カードUART経路、設定値、公開APIの契約は変更対象外です。

## 残した処理と理由

| 処理・状態 | 維持が必要な理由 |
|---|---|
| `SetCapture(false)` | キャプチャ終了時にUSB転送を回収し、配信バッファを停止するためです。選局時のUSB停止・旧TS破棄は上位層で扱う方針に合わせて削除しました。 |
| `streaming_` と `buffer_started_` | USB開始に失敗した場合も配信バッファは稼働するため、再試行時の再初期化を避けるためです。 |
| `stream_buffer_` の共有参照 | `Term` がデバイスロックを解いて `Receiver` の終了を待つ間も、抜去通知と受信コールバックがバッファを参照するためです。 |
| `terminating_` とロックを解いてからの登録解除 | 終了待機中の新規要求を拒否し、ReceiverManagerからデバイスへのロック取得順と逆転させないためです。 |
| `card_open_` と `receiver_open_` | カードと受信機のどちらかが利用中なら電源を保持する必要があります。H2の再投入はチューナーをリセットします。 |
| 初期化途中の解放条件 | 復調器・チューナーのどこまで初期化に成功したかで解放可能な範囲が変わるためです。 |
| `system_` | 要求中の `params_.system` と、選局に成功した方式を区別するためです。 |
| `XitTsFramer`（後日撤去） | 整理時点では同期回復と端数保持を担当していましたが、利用が不要と判明したため、2026年10月8日に専用試験とともに撤去しました。代わりにISDB2056系と同様の `StreamHandler` / `StreamProcess` を追加し、共通の同期判定関数による通常TSの同期回復とUSB入力間の端数保持を機種実装へ移しました。 |
| GPIO、Sonyチューナー設定、TS暗号化bypass | XIT-SQR100の配線・受信回路・USB転送形式に対応する操作です。他機種の手順への置換には実機の根拠が必要です。 |

従来の選局と終了の分離はコミット `1d837cd` と [TS配信停止の修正記録](20261006_xitsqr100_ts_delivery_fix.md)で確認できます。その後、選局時のUSB停止・旧TSと端数の破棄を上位層へ委ねるため、`SetFrequency` と `SetStreamId` の `PauseCapture` 呼び出しと同関数を削除しました。当時は通常のキャプチャ開始・終了時の端数初期化が残っていましたが、XitTsFramerの撤去後は `remain_len` の初期化へ置き換えました。以下の専用試験の結果は撤去前の記録です。以下の実機計測は、この削除より前の実装に対する結果です。カードの未解決条件は [カード接続エラー調査](20261007_xitsqr100_card_connection_investigation.md)を参照してください。

## 選局前処理削除後の検証

上記のMSBuildコマンドで、v143・Release-staticのDriverHost本体はx64 / x86ともビルドできました。ソリューション全体では `smart_card_state_test` のリンクが `LNK1101: MSPDB140.DLL` のバージョン不一致で失敗したため、全体のビルド成功は未確認です。

`winusb/build/<architecture>/Release-static/` の `xit_ts_framer_test.exe`、`ts_sync_condition_test.exe`、`stream_buffer_retune_test.exe`、`ringbuffer_purge_test.exe` はx64 / x86とも成功し、`git diff --check` も成功しました。これらは端数処理・同期条件・バッファの自動試験であり、削除後の実機選局、TS連続性、カード同時負荷、長時間受信は未確認です。

## 削除前のビルドと自動試験

リポジトリルートから次のビルドがx64 / x86とも成功しました。

```powershell
MSBuild winusb/px4_winusb.sln /p:Configuration=Release-static /p:Platform=x64 /p:PlatformToolset=v143 /v:minimal /nologo
MSBuild winusb/px4_winusb.sln /p:Configuration=Release-static /p:Platform=x86 /p:PlatformToolset=v143 /v:minimal /nologo
```

`winusb/build/<architecture>/Release-static/` の `xit_ts_framer_test.exe`、`ts_sync_condition_test.exe`、`stream_buffer_retune_test.exe`、`smart_card_state_test.exe`、`ringbuffer_purge_test.exe` をx64 / x86で実行し、すべて成功しました。`git diff --check` も成功しました。

配布物の再生成は今回の整理の対象外のため、`build.ps1` 全体の配布・カタログ検証工程は実行対象外です。

## 実機試験

更新したx64 DriverHostとBonDriverを独立した検証用ディレクトリへコピーし、既存DriverHostが起動していないことを確認してから試験用ホストを起動しました。アプリの配置ファイルは変更していません。

```powershell
.\channel_switch_stress.exe .\BonDriver_XIT-SQR100.dll 0 0 0 6 3000
.\channel_switch_stress.exe .\BonDriver_XIT-SQR100.dll 1 0 0 4 3000
.\channel_switch_stress.exe .\BonDriver_XIT-SQR100.dll 2 0 0 4 3000
.\card_stream_stress.exe .\BonDriver_XIT-SQR100.dll 0 0 5 10 - XIT-SQR100
```

各再選局の計測は3秒です。同一チャンネルの再選局と通常のCloseTuner / Releaseを確認しました。

| 対象 | 再選局回数 | TSパケット数 | 同期エラー | TEI | 連続性エラー | 空受信周期 |
|---|---:|---:|---:|---:|---:|---:|
| 地デジ13Ch | 6 | 357376 | 0 | 0 | 0 | 0 |
| BS01/TS0 | 4 | 135168 | 0 | 0 | 0 | 0 |
| CS space 2 / channel 0 | 4 | 414720 | 0 | 0 | 0 | 0 |

選局失敗はすべて0で、各試験の終了コードは0でした。衛星の信号値はBS 18.70、CS 16.60～16.70でした。

カード同時負荷試験は、接続2回とも既知の `0x80100016` で失敗し、終了コード1でした。APDU成功0回・失敗2回のため、正常なカード通信との同時受信は未確認です。受信のみ5秒で99328パケット、カード接続試行を含む10秒で199680パケットを取得し、双方の同期エラー・TEI・連続性エラーは0でした。この結果はカード接続失敗時の受信継続を示すもので、カード通信の成功試験ではありません。

PXMLTの実機、x86での実機受信、異なるチャンネル間の切り替え、カード接続を保持した共有電源、USB抜去、長時間録画、外付けカードリーダー、TVTestのD/E/Sは未確認です。
