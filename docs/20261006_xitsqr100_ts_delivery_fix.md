# XIT-SQR100 の WinUSB TS 配信停止の修正と検証

検証環境は Windows、Visual Studio 2022 v143、USB 接続の PIXELA XIT-SQR100 1台です。実機計測は x64 BonDriver と x64 DriverHost、内蔵カードを使いました。地デジ・BS・CS のアンテナ接続は前回検証記録の利用者申告によるもので、今回の計測でも各方式の TS を取得できました。外付けカードリーダーは今回の検証対象外です。検証日は2026年10月6日です。

## 原因と変更範囲

BonDriver は OpenTuner でキャプチャとパイプ配信を開始します。XIT-SQR100 の SetFrequency と SetStreamId は選局前に SetCapture(false) を呼んでいました。この呼び出しは StreamBuffer::Stop により配信スレッドの HandleRead も終了させるため、選局後に USB 受信とバッファを再開してもパイプへ TS を送る処理が残りませんでした。信号値の取得は別の制御経路なので、信号値が得られても TS は0パケットになります。

選局用の PauseCapture では USB 受信スレッドを回収し、リングバッファと TS フレーマーの端数を破棄します。配信スレッドが継続できるよう、バッファの停止はキャプチャ終了時に限定しました。選局後は既存バッファを使い、USB 受信だけを再開します。USB 開始失敗時にも配信を終了させず、エラーを呼び出し元へ返すため、次の選局を試せます。共有 StreamBuffer、他機種、公開 API の契約に変更はありません。

## ビルドと自動試験

リポジトリルートから次を実行し、双方のソリューションビルドが成功しました。

```powershell
MSBuild winusb/px4_winusb.sln /p:Configuration=Release-static /p:Platform=x64 /p:PlatformToolset=v143
MSBuild winusb/px4_winusb.sln /p:Configuration=Release-static /p:Platform=x86 /p:PlatformToolset=v143
MSBuild winusb/tests/stream_buffer_retune_test.vcxproj /p:Configuration=Release-static /p:Platform=x64 /p:PlatformToolset=v143
MSBuild winusb/tests/stream_buffer_retune_test.vcxproj /p:Configuration=Release-static /p:Platform=Win32 /p:PlatformTarget=x86 /p:PlatformToolset=v143
```

stream_buffer_retune_test は実際の StreamBuffer に20回の破棄と新しいパケットの投入を行い、同一配信スレッドが継続し、最後の StopRequest で終了することを確認します。x86 / x64 の双方で成功しました。smart_card_state_test、ts_sync_condition_test、xit_ts_framer_test、ringbuffer_purge_test も両アーキテクチャで成功しました。git diff --check も成功しました。

## 実機計測

修正ビルドを独立した検証用ディレクトリへ配置し、そこで以下を実行しました。各再選局の計測時間は3秒です。

```powershell
.\channel_switch_stress.exe .\BonDriver_XIT-SQR100.dll 0 0 0 6 3000
.\channel_switch_stress.exe .\BonDriver_XIT-SQR100.dll 1 0 0 4 3000
.\channel_switch_stress.exe .\BonDriver_XIT-SQR100.dll 2 0 0 4 3000
.\card_stream_stress.exe .\BonDriver_XIT-SQR100.dll 0 0 5 30 - XIT-SQR100
```

| 対象 | 再選局回数 | TS パケット数 | 同期エラー | TEI | 連続性エラー | 空受信周期 |
|---|---:|---:|---:|---:|---:|---:|
| 地デジ13Ch | 6 | 358400 | 0 | 0 | 0 | 0 |
| BS01/TS0 | 4 | 135168 | 0 | 0 | 0 | 0 |
| CS space 2 / channel 0 | 4 | 413696 | 0 | 0 | 0 | 0 |

上記の選局失敗数はすべて0です。最初の探索試験では地デジ channel 0 から59392パケットを取得できましたが、channel 1 は選局に失敗しました。このチャンネル間の切り替え成功は未確認です。

カード同時負荷では受信のみ5秒で99328パケット、APDU通信との同時受信30秒で598016パケットを取得しました。同期エラー・TEI・連続性エラーは両区間とも0、APDU成功343回・失敗0回、終了時信号値30.57でした。このツールから APDU 最大処理時間は取得できません。TVTest の D/E/S と復号結果は今回の計測対象外です。

修正後の DriverHost_PX4.exe を dist の XIT-SQR100 用32bit / 64bitディレクトリへ反映しました。TVTest のインストール先への適用と画面上の Mbps、異なる放送チャンネルや方式間の連続切り替え、長時間録画、USB 抜去、他機種・他ロットは未確認です。試験は通常の CloseTuner / Release で終了しました。
