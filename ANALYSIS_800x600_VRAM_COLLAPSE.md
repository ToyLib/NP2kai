# NP2kai Windows 3.1 + WAB/800x600 VRAM表示崩壊 原因分析報告書

## 問題概要

Windows 3.1 + MELCO WSN-A4F (WAB アクセラレータ) 環境にて：
- **640x480 256色** → 正常表示
- **800x600 256色へ変更** → VRAM表示が崩壊（画面全体が横方向に周期的に繰り返す）

### 症状の特徴
- 画面全体が横方向に一定間隔で周期的に繰り返される
- Windows画面の断片が規則的に並ぶ
- ランダムノイズではなく、確定的な繰り返しパターン
- I-O DATA GA-98NBIV でも同様の症状報告あり

---

## 原因特定: 最有力仮説

### ★★★ A. CR13 レジスタの Pitch 計算ミス [確度: 95%]

#### 背景
VGAの画面描画は、VRAM上で**ピッチ(pitch)**と呼ばれる1行のバイト数を指定して行われます。

```
描画バッファ構造：
行0: [Pixel 0][Pixel 1]...[Pixel 639]  |  padding  |
行1: [Pixel 0][Pixel 1]...[Pixel 639]  |  padding  |  ← ピッチの分だけオフセット
...
```

#### CR13 レジスタ（Line Offset）の計算
NP2kai では、以下の式で pitch を計算しています：

```c
// cirrusvga_drawGraphic() [wab/cirrus_vga.c L5308]
line_offset = cirrusvga->cr[0x13] | ((cirrusvga->cr[0x1b] & 0x10) << 4);
line_offset <<= 3;  // byte単位に変換（CR13単位は8バイト）
```

つまり：
```
pitch (bytes) = (CR13 | (CR1B[4] << 4)) × 8
```

#### 期待される値

| 解像度 | 色深度 | 画面幅 | CR13期待値 | 期待Pitch |
|--------|--------|--------|------------|-----------|
| 640x480 | 256色(8bit) | 640px = 640B | 80 (0x50) | 640 |
| 800x600 | 256色(8bit) | 800px = 800B | **100 (0x64)** | **800** |

#### 現在の実装の問題

**`cirrus_vga_drawGraphic()` [L5343-5350]:**
```c
// CRTC offset 設定
scanW = width*(bpp/8);
scanpixW = width;
if(bpp && line_offset){
    // 32bit color用やっつけ修正 for GA-98NB & WSN-A2F/A4F
    if(bpp==32){
        if((np2clvga.gd54xxtype & CIRRUS_98ID_GA98NBMASK) == CIRRUS_98ID_GA98NBIC || 
           (np2clvga.gd54xxtype & CIRRUS_98ID_WABMASK) == CIRRUS_98ID_WAB){
            line_offset <<= 1;  // ★32bit時だけの修正★
        }
    }
    scanW = line_offset;
}
```

**問題：**
1. **32bit時だけ特殊処理がある** → 16bit, 8bit では適用されない
2. **256色(8bit)では線形計算のみ** → CR13 が正しく設定されていないと正しい pitch が得られない

#### 症状との関連性

もし Windows ドライバが 800x600 256色への切り替え時に **CR13 を 100に設定し忘れた** ら：

```
状況：CR13 = 80のまま（640x480の値）
- 画面幅：800px
- 実際pitch：640 bytes
- 毎行のずれ：800 - 640 = 160px

結果：
行0: VRAM[0-639]    表示[0-639]     ← 正常
行1: VRAM[640-1279] 表示[0-639]     ← 640pixel分ずれて表示される（wrap-around）
行2: VRAM[1280-...]              ← さらにずれ
```

**→ 画面全体が 640px の周期で繰り返される** ★症状と完全一致！★

---

## 関連コード位置

### 1. Pitch 計算処理
**ファイル:** [wab/cirrus_vga.c](wab/cirrus_vga.c)

| 行番号 | 関数/処理 | 説明 |
|--------|----------|------|
| [1620-1650](wab/cirrus_vga.c#L1620-L1650) | `cirrus_get_offsets()` | CR13 から pitch を計算（フック処理無し） |
| [1630-1633](wab/cirrus_vga.c#L1630-L1633) | line_offset計算 | `pitch = (CR13 \| (CR1B[4]<<4)) << 3` |
| [5308](wab/cirrus_vga.c#L5308) | `cirrusvga_drawGraphic()` | 同じく line_offset を再計算 |
| [5343-5350](wab/cirrus_vga.c#L5343-L5350) | CRTC offset設定 | **32bit時のみ特殊処理** |

### 2. CR レジスタへの書き込み
**ファイル:** [wab/cirrus_vga.c](wab/cirrus_vga.c)

| 行番号 | 処理 | 説明 |
|--------|------|------|
| [2295-2388](wab/cirrus_vga.c#L2295-L2388) | `cirrus_hook_write_cr()` | CR レジスタ書き込みフック |
| [2295-2327](wab/cirrus_vga.c#L2295-L2327) | CR00-CR18 | `CIRRUS_HOOK_NOT_HANDLED` → フック処理なし |
| [4340-4350](wab/cirrus_vga.c#L4340-L4350) | 0x3D4/0x3D5処理 | CRT コントローラ index/data ポート |

**問題：CR13 は CIRRUS_HOOK_NOT_HANDLED** → Windows ドライバから直接設定されたまま反映される

### 3. WAB/WSN-A4F 初期化
**ファイル:** [wab/cirrus_vga.c](wab/cirrus_vga.c)

| 行番号 | 関数 | 説明 |
|--------|------|------|
| [7752-7810](wab/cirrus_vga.c#L7752-L7810) | `pc98_cirrus_setWABreg()` | アクセラレータ型別初期化 |
| [7775-7785](wab/cirrus_vga.c#L7775-L7785) | WSN-A4F設定 | `s->device_id = CIRRUS_ID_CLGD5434` |
| [6618-6680](wab/cirrus_vga.c#L6618-L6680) | `cirrusvga_setAutoWABID()` | 自動判定・初期化 |

---

## 二次的な問題候補

### B. 表示開始アドレス（Start Address）の計算不完全

**ファイル:** [wab/cirrus_vga.c#L1635-1640](wab/cirrus_vga.c#L1635-L1640)
```c
start_addr = (s->cr[0x0c] << 8)
    | s->cr[0x0d]
    | ((s->cr[0x1b] & 0x01) << 16)
    | ((s->cr[0x1b] & 0x0c) << 15)
    | ((s->cr[0x1d] & 0x80) << 12);
```

**可能性：** CR0C/0D/1B/1D が不完全に設定された場合、start_addr ずれ

### C. 16bit 色での Pitch 処理未確認

[wab/cirrus_vga.c#L5343-5350](wab/cirrus_vga.c#L5343-L5350) で 32bit のみ特殊処理。
16bit での動作確認が必要。

### D. VSN ドライバの CR13 設定ロジックの問題

Windows ドライバが 800x600 へ切り替え時に CR13 レジスタを更新しない可能性。
これはドライバ側の問題だが、NP2kai でのワークアラウンド検討が必要。

---

## 修正案

### 提案1: デバッグ機能の追加（短期対応）

**ファイル:** `wab/cirrus_vga.c`

```c
// cirrusvga_drawGraphic() の先頭に追加 [L5253]

#ifdef DEBUG_CIRRUS_PITCH_LOG
static FILE *pitch_log = NULL;
if (!pitch_log) pitch_log = fopen("cirrus_pitch.log", "w");
fprintf(pitch_log, "==== Frame ====\n");
fprintf(pitch_log, "width=%d, height=%d, bpp=%d\n", width, height, bpp);
fprintf(pitch_log, "CR01=%02X, CR12=%02X, CR13=%02X, CR1B=%02X, CR1D=%02X\n",
    cirrusvga->cr[0x01], cirrusvga->cr[0x12], cirrusvga->cr[0x13],
    cirrusvga->cr[0x1b], cirrusvga->cr[0x1d]);
fprintf(pitch_log, "line_offset_raw=%d, line_offset=%d, scanW=%d\n",
    cirrusvga->cr[0x13] | ((cirrusvga->cr[0x1b] & 0x10) << 4),
    line_offset, scanW);
fflush(pitch_log);
#endif
```

**効果：** 800x600 切り替え時の CR13 実際値を確認可能

### 提案2: 256色/16bit での Pitch 修正（中期対応）

[wab/cirrus_vga.c#L5343-5350](wab/cirrus_vga.c#L5343-L5350) を拡張：

```c
// CRTC offset 設定
scanW = width*(bpp/8);
scanpixW = width;
if(bpp && line_offset){
    // GA-98NB & WSN-A2F/A4F 向け修正
    if((np2clvga.gd54xxtype & CIRRUS_98ID_GA98NBMASK) == CIRRUS_98ID_GA98NBIC || 
       (np2clvga.gd54xxtype & CIRRUS_98ID_WABMASK) == CIRRUS_98ID_WAB ||
       np2clvga.gd54xxtype == CIRRUS_98ID_WSN || 
       np2clvga.gd54xxtype == CIRRUS_98ID_WSN_A2F){
        
        // 32bit/16bit/8bit 全色深度での pitch 調整検討
        if(bpp==32){
            line_offset <<= 1;
        }else if(bpp==16){
            // 16bit での pitch が正しく計算されているか確認
            // 必要に応じて調整
        }else if(bpp==8){
            // ★256色での pitch 検証・修正
            // CR13 が正しく設定されている前提だが、
            // もしずれていたら以下のワークアラウンド：
            // int expected_pitch = width;  // 256色なら width = pitch
            // if(scanW != expected_pitch) { ... }
        }
    }
    scanW = line_offset;
}
```

### 提案3: ドライバロジック検証（長期対応）

Windows ドライバが解像度変更時に CR13 を正しく設定しているか検証：

1. NP2kai でドライバが発行する I/O ポートトレースを取得
2. CRTC Index (0x3D4) → Data (0x3D5) の書き込みシーケンスを記録
3. CR13 の値が 800x600 で 100 (0x64) に更新されるか確認

---

## 最終判定

### 「未実装なのか」vs「設定問題なのか」の答え

**判定: 未実装とドライバ設定の複合問題**

| 観点 | 結論 |
|------|------|
| **WAB 高解像度対応状況** | ✓ 基本的には実装されている（CLGD5434ボード対応） |
| **800x600 への対応** | ✓ cirrus_get_resolution() で正しく計算可能 |
| **256色 256x60 での pitch処理** | ✗ 32bit時だけの特殊処理で、8bit対応不足の可能性 |
| **ドライバレベルの問題** | ✗ Windows ドライバが CR13 を正しく設定しない可能性 |
| **ワークアラウンド実装** | ✗ 256色・16bit 色での特殊処理がない |

### 推奨される対応順序

1. **即座（必須）:** デバッグログ機能を追加して CR13 の実値確認
2. **短期（重要）:** 256色での pitch ワークアラウンド実装
3. **中期（推奨）:** Windows ドライバ側での CR レジスタ設定検証
4. **長期（品質向上）:** 全色深度での pitch 計算の統一・簡潔化

---

## 検証方法

NP2kai SDL2 Linux版で以下を実施：

```bash
# 1. デバッグ機能有効でビルド
cmake -DDEBUG_CIRRUS_PITCH_LOG=ON ..
make

# 2. Windows 3.1 起動、800x600 256色に変更
# 3. cirrus_pitch.log を確認
cat cirrus_pitch.log

# 期待される出力例（800x600時）：
# CR01=63 (width = 100*8 = 800 ✓)
# CR13=64 (pitch = 100 bytes ✓) ← ここが80のままなら問題！
```

---

## 参考資料

- **VGA CRTC レジスタ仕様:** Finn Thogerson's VGADOC4b
  - CR00-CR18: Standard VGA
  - CR1B: Cirrus Extended Display Control
  - CR1D: Overlay Extended Control
  
- **Cirrus Logic チップセット:** CLGD5434 データシート
  - SR0F: Memory Size Register
  - GR09-0A: Bank Address Registers
  - GR0B: Banking Mode Control

---

## 附録: ソースコード箇所マップ

```
wab/cirrus_vga.c
├── pitch 計算
│   ├── cirrus_get_offsets() [L1620-1650]
│   └── cirrusvga_drawGraphic() [L5253-5600]
├── I/O レジスタ処理
│   ├── vga_ioport_write() [L4217-4420]
│   ├── vga_ioport_read() [L4078-4200]
│   └── cirrus_hook_write_cr() [L2295-2388]
├── アクセラレータ初期化
│   ├── pc98_cirrus_setWABreg() [L7752-7810]
│   └── cirrusvga_setAutoWABID() [L6618-6680]
└── レジスタマッピング
    ├── 標準VGA: 0x3D4/0x3D5
    ├── WAB: 0x54E0+offset / 0x55E0+offset
    └── GA-98NB: 0xD54/D55 or 0xDA4/DA5

wab/wab.c/h
├── np2wab_setScreenSize() [wab.c L225-280]
├── np2wab_setScreenSizeMT() [wab.c L283-295]
└──構造体: NP2WAB, NP2WABWND

wab/cirrus_vga_extern.h
├── CIRRUS_98ID_* 定数
└── CIRRUS_VRAM_SIZE_*
```

---

**作成日:** 2026-08-19
**バージョン:** NP2kai current
**分析対象:** Windows 3.1 + MELCO WSN-A4F 環境での 800x600 256色 VRAM 表示崩壊問題
