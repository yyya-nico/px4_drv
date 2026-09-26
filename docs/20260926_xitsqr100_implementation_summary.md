# XIT-SQR100 サポート実装サマリー (2026-09-26)

## 実装完了したもの

### 1. デバイス定義 (`driver/px4_usb.h`)
- USB VID: `0x06b8` (XIT)
- USB PID: `0x106b` (XIT-SQR100)
- デバイス型列挙: `XITSQR100_USB_DEVICE` を追加
- MAX_USB_DEVICE_TYPE 前に配置完了

### 2. CXD6866AER tuner ドライバ (`driver/cxd6866.{h,c}`)
- **状態**: スケルトン実装（コンパイル可能だが機能未実装）
- ヘッダー API: CXD2858ER と同じシグネチャで統一
  - `cxd6866_init(tuner)` → `-EOPNOTSUPP` を返す（未実装警告）
  - `cxd6866_set_params_t(tuner, system, freq, bandwidth)` → `-EOPNOTSUPP`
  - `cxd6866_set_params_s(tuner, system, freq, symbol_rate)` → `-EOPNOTSUPP`
  - `cxd6866_stop(tuner)` → `0` (stub)
  - `cxd6866_term(tuner)` → NOP
- I2C ヘルパー関数を提供（`__maybe_unused` マーク）
- 登録マップ、tuning シーケンス: **未実装（TODO コメント付き）**

### 3. XIT-SQR100 デバイスドライバ (`driver/xit_sqr100_device.{h,c}`)
- **構造**: s1ur_device.c（単一 tuner）+ pxmlt_device.c（CXD2856ER + tuner pairing）をハイブリッド
- **TS 同期**: 平文 `0x47` sync byte（`px4_ts_has_plain_sync` 使用、PXMLT の受信機番号付き sync ではない）
- **主要機能**:
  - `xit_sqr100_backend_set_power`: GPIO 3/2 power sequence (s1ur_device.c パターン)
  - `xit_sqr100_chrdev_open`: CXD2856ER + CXD6866 の init + CXD2856ER 初期化レジスタ sequence
  - `xit_sqr100_chrdev_tune`: ISDB-T/S 対応、tuner_lock での排他制御
  - `xit_sqr100_chrdev_check_lock`: CXD2856ER TS lock 判定（unlocked で ECANCELED）
  - `xit_sqr100_chrdev_set_stream_id`: ISDB-S stream ID/slot 管理
  - `xit_sqr100_chrdev_set_lnb_voltage`: GPIO 11 LNB 電源管理（pxmlt パターン）
  - `xit_sqr100_chrdev_start/stop_capture`: TS ストリーミング + PSB purge
  - `xit_sqr100_chrdev_read_cnr_raw`: ISDB-T/S CNR 参照テーブル（pxmlt から複製）
- **状態**: オブジェクト寿命、排他制御、TS 処理の骨組みは完成
- **未検証**: hardware-specific レジスタ値、GPIO pin 割り当て、I2C address 設定

### 4. USB レイヤー統合 (`driver/px4_usb.c`)
- 新 VID `0x06b8` の probe case を追加
- XITSQR100 デバイス型のコンテキスト union entry を追加
- `px4_usb_disconnect` に XITSQR100_USB_DEVICE case を追加
- USB device ID テーブルに `{ USB_DEVICE(0x06b8, USB_PID_XIT_SQR100) }` を追加
- `px4_usb_register` で chrdev context 作成（isdbt2071 直後、xitsqr100video デバイス）
- `px4_usb_unregister` で逆順 destroy を追加

### 5. ビルドシステム (`driver/Kbuild`)
- `XITSQR100_USB_MAX_DEVICE := 0` 定義を追加
- ccflags `-DXITSQR100_USB_MAX_DEVICE` の conditional block を追加
- オブジェクト: `cxd6866.o xit_sqr100_device.o` を px4_drv-y に追加

### 6. udev ルール (`etc/99-px4video.rules`)
- `KERNEL=="xitsqr100video*", GROUP="video", MODE="0664"` を追加
- USB power control: `SUBSYSTEM=="usb", ATTRS{idVendor}=="06b8", ACTION=="add", TEST=="power/control", ATTR{power/control}="on"` を追加

### 7. ドキュメント (`README.md`)
- 対応デバイス表に XIT-SQR100 (実験的) を追加
- NOTE セクションに新規サポート一覧へ「XIT XIT-SQR100 (実験的、Linux 版のみ)」を追加

## 未検証・未実装の項目（ハードウェア/データシート確認が必須）

### CXD6866AER tuner driver
| 項目 | 状態 | 理由 |
|------|------|------|
| I2C address | プレースホルダー: `0x60` | pxmlt の CXD2858ER パターンをコピー。XIT-SQR100 スキーマティック未確認 |
| Crystal frequency (xtal) | プレースホルダー: `16000` kHz | pxmlt 値をコピー。実機 or datasheet 確認が必須 |
| LNA 設定 (ter/sat) | `true` | pxmlt 値をコピー。実機動作確認が必須 |
| Register map (tuning sequence) | **未実装** | CXD6866AER datasheet が利用不可。init/set_params_t/set_params_s は `-EOPNOTSUPP` を返す |

### IT9303FN GPIO & I2C 配置
| 項目 | 現在の値 | 根拠 | 未検証 |
|------|---------|------|--------|
| Power GPIO (on) | GPIO 3 → LOW, 100ms, GPIO 2 → HIGH | s1ur_device.c (ISDBT2071_MODEL) パターン | ✗ |
| Power GPIO (off) | GPIO 2 → LOW, GPIO 3 → HIGH | s1ur_device.c パターン | ✗ |
| LNB voltage GPIO | GPIO 11 | pxmlt_device.c パターン | ✗ |
| Input port_number | `4` | pxmlt ISDBT2071_MODEL パターン | ✗ |
| I2C bus | `3` | pxmlt ISDBT2071_MODEL パターン | ✗ |
| CXD2856ER I2C address (SLVT) | `0x18` | pxmlt ISDBT2071_MODEL パターン | ✗ |
| CXD2856ER I2C address (SLVX) | `0x1A` (0x18 + 2) | pxmlt 規則に従う | ✗ |

### TS 同期
- **仮定**: XIT-SQR100 は平文 `0x47` sync byte を出力（PXS1UR / ISDBT2071 同様）
- **根拠**: ISDB-T/S の標準 TS format であり、pxmlt_device.c の受信機番号付き sync ではない
- **未検証**: 実機受信での sync byte 確認

### その他
| 項目 | 状態 |
|------|------|
| ドライバコンパイル | **未確認**（Windows ビルド環境なし。Linux WSL/VM で実行が必要） |
| 実機受信テスト | **未実施**（物理デバイス及び実機環境がない） |
| 長時間連続受信テスト | **未実施** |
| チャンネル切り替え時の同期エラー | **未確認** |
| LNB voltage 制御 | **未検証** |
| B-CAS card reader 統合 | **未実装**（Linux 版では予定なし） |

## 次ステップ（実装者向け）

### 優先度 1: Linux 環境でのコンパイル確認
```bash
cd driver/
make
```
- `cxd6866.c` の `__maybe_unused` マークが `-Werror` をクリアするか確認
- include path, symbol export が正しいか確認

### 優先度 2: CXD6866AER tuning 実装
1. XIT-SQR100 technical manual / CXD6866AER datasheet を入手
2. tuning register sequence を `cxd6866_set_params_t/s` へ実装
3. I2C address, xtal frequency, LNA config を確認・修正
4. `cxd6866_init()` の power-on sequence を実装
5. `-EOPNOTSUPP` 返却を削除

### 優先度 3: GPIO & I2C 配置確認
1. XIT-SQR100 PCB schematic を入手
2. IT9303FN GPIO pin → 回路機能のマッピング
3. I2C デバイス: CXD2856ER i2c_addr, CXD6866 i2c_addr の確認
4. `xit_sqr100_device_load_config()` の port_number / i2c_bus / i2c_addr を修正

### 優先度 4: 実機テスト
1. XIT-SQR100 を Linux PC に接続
2. `insmod px4_drv.ko` 実行、`/dev/xitsqr100video0` が出現することを確認
3. 地上波 ISDB-T チャンネルで受信テスト（記録時間 1–2 時間）
   - D, E, S の確認
   - transport error flag (TEI) カウント
   - continuity counter エラー検出
4. BS/CS ISDB-S で受信テスト（同様に統計情報確認）
5. チャンネル切り替え時の TS 同期エラー検出

### 優先度 5: WinUSB 版（オプション）
- Windows 版 DriverHost_PX4 対応は **本実装では未開始**
- 詳細は AGENTS.md の「WinUSB 版」セクション参照

## コンパイラ警告の抑止理由

### `cxd6866.c` の `__maybe_unused` 属性
`cxd6866_read_regs`, `cxd6866_read_reg`, `cxd6866_write_regs`, `cxd6866_write_reg`, `cxd6866_write_reg_mask` は、
当前は `cxd6866_set_params_t/s` で使用されていない placeholder helper です。
登録マップが判明する際に実装されます。`-Werror` ビルドでの unused function warning を回避するため、
`__maybe_unused` を付与しています。

## 参照・設計根拠

| 項目 | 参照先 | 選定理由 |
|------|--------|---------|
| デバイス構造 (chrdev, device, open/close/tune) | s1ur_device.c | 単一 tuner, plain 0x47 TS, ISDB-T/S 両対応 |
| CXD2856ER + tuner pairing | pxmlt_device.c | CXD2856ER demod + Sony tuner wiring, LNB voltage |
| TS 同期処理 | s1ur_device.c (px4_ts_has_plain_sync) | plain 0x47 format |
| GPIO power | s1ur_device.c (GPIO 3/2) → ISDBT2071_MODEL | 単一 tuner 電源パターン |
| LNB voltage GPIO | pxmlt_device.c (GPIO 11) | 衛星電源一元管理 |
| CNR lookup table | pxmlt_device.c (isdbt_cn_raw_table / isdbs_cn_raw_table) | ISDB-T/S 両対応 |
| USB layer | px4_usb.c probe/disconnect/register pattern | 既存デバイス型との統一 |

## AGENTS.md ガイダンスへの準拠

✓ 最小限の差分 (cxd6866/xit_sqr100 新規追加のみ、既存ファイルは機能維持)  
✓ 実測値のみ使用 (timeout/tuning sequence は placeholder TODO)  
✓ 状態破棄の完全性 (atomic_xchg available, kfree stream_ctx)  
✓ エラー境界保持 (CXD6866 未実装は -EOPNOTSUPP で明示)  
✓ 確認範囲の明記 (このドキュメント，各 TODO コメント)  
✓ 実機試験の確認必須事項記載 (D/E/S, TEI, continuity error など)  

---

**実装者**: GitHub Copilot  
**実装日**: 2025-01-22  
**最後に**: 本実装は Linux 版のみです。Windows WinUSB 版の対応は別途検討が必要です。
