# NP2kai 800x600 256色 VRAM 崩壊問題 - 修正実装提案

## 修正案1: デバッグ機能の追加

**ファイル:** `wab/cirrus_vga.c`

### Step 1: ヘッダーに DEBUG マクロ定義を追加

[wab/cirrus_vga.c の最初の部分に追加]

```c
// デバッグ用マクロ（開発時のみ有効）
#ifdef DEBUG_CIRRUS_PITCH
#include <stdio.h>
static FILE *g_cirrus_pitch_log = NULL;
#define CIRRUS_PITCH_LOG(fmt, ...) \
    do { \
        if (!g_cirrus_pitch_log) { \
            g_cirrus_pitch_log = fopen("cirrus_pitch_debug.log", "w"); \
        } \
        if (g_cirrus_pitch_log) { \
            fprintf(g_cirrus_pitch_log, fmt "\n", ##__VA_ARGS__); \
            fflush(g_cirrus_pitch_log); \
        } \
    } while(0)
#else
#define CIRRUS_PITCH_LOG(fmt, ...)  /* disabled */
#endif
```

### Step 2: cirrusvga_drawGraphic() に debug logging を追加

[wab/cirrus_vga.c L5253 の `int cirrusvga_drawGraphic(){` の直後]

```c
int cirrusvga_drawGraphic(){
//#define DEBUG_CIRRUS_VRAM
#if defined(DEBUG_CIRRUS_VRAM)
	// ... 既存コード ...
#endif

	int i, j, width, height, bpp;
	uint32_t_ line_offset = 0;
	
	// ★★★ 追加: デバッグログ初期化 ★★★
	static int frame_count = 0;
	frame_count++;
	CIRRUS_PITCH_LOG("========== Frame %d ==========", frame_count);
	
	// ... 既存処理 ...
	
	// L5308 の line_offset 計算の直後に追加
	line_offset = cirrusvga->cr[0x13] | ((cirrusvga->cr[0x1b] & 0x10) << 4);
	line_offset <<= 3;
	
	// ★★★ 追加: CR レジスタダンプ ★★★
	CIRRUS_PITCH_LOG("CRT Register Values:");
	CIRRUS_PITCH_LOG("  CR01 (Horiz End)     = 0x%02X", cirrusvga->cr[0x01]);
	CIRRUS_PITCH_LOG("  CR12 (Vert End)      = 0x%02X", cirrusvga->cr[0x12]);
	CIRRUS_PITCH_LOG("  CR13 (Line Offset)   = 0x%02X (raw)", cirrusvga->cr[0x13]);
	CIRRUS_PITCH_LOG("  CR1B (Extended Ctrl) = 0x%02X", cirrusvga->cr[0x1b]);
	CIRRUS_PITCH_LOG("  CR1D (Overlay Ctrl)  = 0x%02X", cirrusvga->cr[0x1d]);
	
	CIRRUS_PITCH_LOG("Calculated Values:");
	CIRRUS_PITCH_LOG("  width  = (%d + 1) * 8 = %d", cirrusvga->cr[0x01], width);
	CIRRUS_PITCH_LOG("  height = %d", height);
	CIRRUS_PITCH_LOG("  bpp    = %d", bpp);
	CIRRUS_PITCH_LOG("  line_offset (raw) = 0x%02X | (0x%02X[4] << 4) = 0x%02X",
		cirrusvga->cr[0x13], cirrusvga->cr[0x1b], 
		cirrusvga->cr[0x13] | ((cirrusvga->cr[0x1b] & 0x10) << 4));
	CIRRUS_PITCH_LOG("  line_offset (byte) = %d (pitch)", line_offset);
	
	// ... 以下既存処理 ...
```

### Step 3: CRTC offset 設定部分にも logging 追加

[wab/cirrus_vga.c L5343 付近]

```c
	// CRTC offset 設定
	scanW = width*(bpp/8);
	scanpixW = width;
	
	CIRRUS_PITCH_LOG("Initial scanW = width * (bpp/8) = %d * (%d/8) = %d", 
		width, bpp, scanW);
	
	if(bpp && line_offset){
		// 32bit color用やっつけ修正 for GA-98NB & WSN-A2F/A4F
		if(bpp==32){
			if((np2clvga.gd54xxtype & CIRRUS_98ID_GA98NBMASK) == CIRRUS_98ID_GA98NBIC || 
			   (np2clvga.gd54xxtype & CIRRUS_98ID_WABMASK) == CIRRUS_98ID_WAB){
				line_offset <<= 1;
				CIRRUS_PITCH_LOG("Applied 32bit correction: line_offset *= 2");
			}
		}
		scanW = line_offset;
		
		CIRRUS_PITCH_LOG("Final scanW (VRAM pitch) = %d", scanW);
		
		// ★★★ 追加: pitch 妥当性チェック ★★★
		int expected_pitch = width * (bpp / 8);
		if (scanW != expected_pitch) {
			CIRRUS_PITCH_LOG("WARNING: Pitch mismatch! Expected %d, Got %d", 
				expected_pitch, scanW);
		}
	}
```

### ビルド方法

```bash
# デバッグ機能を有効にしてビルド
cd /path/to/NP2kai
mkdir build_debug && cd build_debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
# もしくは直接コンパイルフラグを指定
gcc -DDEBUG_CIRRUS_PITCH ... wab/cirrus_vga.c ...

# 実行
./sdlnp21kai

# Windows 3.1 で 800x600 256色に変更
# cirrus_pitch_debug.log を確認
cat cirrus_pitch_debug.log
```

---

## 修正案2: 256色での Pitch 補正ロジック

**ファイル:** `wab/cirrus_vga.c`

[wab/cirrus_vga.c L5343-5360 を以下に置き換え]

```c
	// CRTC offset 設定
	scanW = width*(bpp/8);
	scanpixW = width;
	if(bpp && line_offset){
		// GA-98NB & WSN-A2F/A4F (WAB系) 向け修正
		BOOL is_wab_type = (np2clvga.gd54xxtype & CIRRUS_98ID_GA98NBMASK) == CIRRUS_98ID_GA98NBIC ||
		                   (np2clvga.gd54xxtype & CIRRUS_98ID_WABMASK) == CIRRUS_98ID_WAB ||
		                   np2clvga.gd54xxtype == CIRRUS_98ID_WSN ||
		                   np2clvga.gd54xxtype == CIRRUS_98ID_WSN_A2F;
		
		if(is_wab_type){
			// ★★★ 修正: 32bit/16bit/8bit 全色深度への対応 ★★★
			if(bpp==32){
				// 32bit color: pitch が正しく計算されているか確認
				line_offset <<= 1;  // 従来の修正
			}
			else if(bpp==16){
				// 16bit color: pitch チェック
				int expected_pitch_16 = width * 2;
				if(line_offset < expected_pitch_16 * 0.9 || line_offset > expected_pitch_16 * 1.1){
					CIRRUS_PITCH_LOG("16bit pitch correction: %d -> %d", line_offset, expected_pitch_16);
					line_offset = expected_pitch_16;
				}
			}
			else if(bpp==8){
				// ★★★ 新規: 256色での pitch チェック・補正 ★★★
				int expected_pitch_8 = width;  // 256色は 1byte/pixel
				
				// もし CR13 が設定されていないなら（小さすぎるなら）補正
				if(line_offset < expected_pitch_8 * 0.9){
					CIRRUS_PITCH_LOG("8bit pitch correction: %d -> %d (CR13=0x%02X)", 
						line_offset, expected_pitch_8, cirrusvga->cr[0x13]);
					line_offset = expected_pitch_8;
				}
				// 逆に大きすぎる場合は警告
				else if(line_offset > expected_pitch_8 * 1.2){
					CIRRUS_PITCH_LOG("WARNING: 8bit pitch unusually large: %d vs expected %d", 
						line_offset, expected_pitch_8);
				}
			}
		}
		
		scanW = line_offset;
	}
```

### 補正ロジックの詳細解説

```
256色（8bit）の場合：
- 画面幅 W pixel → VRAM上 W byte
- 期待ピッチ = W byte

例：
- 640x480: 期待ピッチ = 640, CR13 = 80, line_offset = 640 ✓
- 800x600: 期待ピッチ = 800, CR13 = ? (100であるべき)
  - もし CR13 = 80のまま → line_offset = 640 ✗
  - この場合、640 < 800 * 0.9 (720) → 補正! 

補正で line_offset = 800 に修正
```

---

## 修正案3: より根本的な修正（推奨）

**考え方:** CR13 レジスタの値を Windows ドライバが正しく設定しているかを監視・検証

**ファイル:** `wab/cirrus_vga.c` の `cirrus_hook_write_cr()` 関数

[wab/cirrus_vga.c L2295 の `cirrus_hook_write_cr()` を改良]

```c
static int cirrus_hook_write_cr(CirrusVGAState * s, unsigned reg_index, int reg_value)
{
    switch (reg_index) {
    case 0x00:			// Standard VGA
    case 0x01:			// Standard VGA
    // ... 他の case 文 ...
    case 0x13:			// ★★★ Line Offset レジスタ ★★★
        {
            // CR13 書き込み時の検証ロジック
            CIRRUS_PITCH_LOG("CR13 write: 0x%02X -> 0x%02X", s->cr[0x13], reg_value);
            
            // 妥当性チェック
            if(s->cr[0x01] > 0){  // CR01 が有効なら（width が設定されているなら）
                int width = (s->cr[0x01] + 1) * 8;  // pixels
                int bpp_detected = cirrus_get_bpp((VGAState *)s);
                int expected_offset = width / 8;  // CR13単位（8バイト単位）
                
                if(bpp_detected > 0){
                    int expected_cr13 = expected_offset * bpp_detected / 8;
                    CIRRUS_PITCH_LOG("  width=%d, bpp=%d, expected_CR13~%d, actual=0x%02X",
                        width, bpp_detected, expected_cr13, reg_value);
                    
                    // 大きく異なる場合は警告
                    if(reg_value > 0 && (reg_value < expected_cr13 * 0.8 || reg_value > expected_cr13 * 1.5)){
                        CIRRUS_PITCH_LOG("  WARNING: CR13 value seems incorrect!");
                    }
                }
            }
            
            // 通常通り書き込み
            s->cr[reg_index] = reg_value;
        }
        return CIRRUS_HOOK_HANDLED;
        
    case 0x19:			// Interlace End
    case 0x1a:			// Miscellaneous Control
    // ... 以降既存コード ...
```

---

## テスト方法

### 1. デバッグログ確認

```bash
# ビルド＆実行
cd build_debug
./sdlnp21kai

# Windows 3.1 起動 → 800x600 256色へ変更

# ログ確認
tail -100 cirrus_pitch_debug.log
```

**期待される出力例（正常時）:**
```
========== Frame 1234 ==========
CRT Register Values:
  CR01 (Horiz End)     = 0x63          ← width = (0x63+1)*8 = 800 ✓
  CR12 (Vert End)      = 0x57
  CR13 (Line Offset)   = 0x64          ← ★ 100 (800>>3) ✓
  CR1B (Extended Ctrl) = 0x10
  CR1D (Overlay Ctrl)  = 0x00

Calculated Values:
  width  = (99 + 1) * 8 = 800
  height = 600
  bpp    = 8
  line_offset (raw) = 0x64 | (0x10[4] << 4) = 0x64
  line_offset (byte) = 800 (pitch)

Initial scanW = width * (bpp/8) = 800 * (8/8) = 800
Final scanW (VRAM pitch) = 800
```

**期待される出力例（問題時）:**
```
========== Frame 1234 ==========
CRT Register Values:
  CR01 (Horiz End)     = 0x63          ← width = 800 ✓
  CR12 (Vert End)      = 0x57
  CR13 (Line Offset)   = 0x50          ← ★★★ 80 のまま (640>>3) ✗✗✗
  CR1B (Extended Ctrl) = 0x10
  CR1D (Overlay Ctrl)  = 0x00

Calculated Values:
  ...
  line_offset (byte) = 640 (pitch)     ← ★★★ 800であるべき ✗✗✗

Initial scanW = width * (bpp/8) = 800 * (8/8) = 800
WARNING: Pitch mismatch! Expected 800, Got 640
Final scanW (VRAM pitch) = 640         ← ★★★ 修正案適用で800に修正される
```

### 2. 修正案検証

修正案2を適用した場合：
```
8bit pitch correction: 640 -> 800 (CR13=0x50)
```

このログが出たら、自動補正が成功している。

---

## 実装チェックリスト

- [ ] デバッグマクロの追加
- [ ] cirrusvga_drawGraphic() へのログ追加
- [ ] コンパイル・テスト
- [ ] 800x600 256色でのログ確認
- [ ] 問題確認後、修正案2の pitch 補正を実装
- [ ] 複数解像度（640x480, 800x600, 1024x768 等）での動作確認
- [ ] 複数色深度（8bit, 16bit, 32bit）での動作確認
- [ ] GA-98NB 系でも動作確認

---

## 参考: VGA CRT コントローラレジスタ一覧

| Register | 説明 | 用途 |
|----------|------|------|
| CR01 | Horizontal Display End | 画面幅 = (CR01+1) * 8 |
| CR12 | Vertical Display End | 画面高さの低8bit |
| CR13 | **Line Offset** | **ピッチ = CR13 << 3 bytes** |
| CR1B | Cirrus Extended Display | CR13 の拡張ビット |
| CR1D | Overlay Extended Control | Start Address拡張 |
| CR0C | Start Address High | 表示開始アドレス上位 |
| CR0D | Start Address Low | 表示開始アドレス下位 |
| CR07 | Vertical Retrace Start | 縦 sync 制御 |

---

**作成日:** 2026-08-19  
**対象:** NP2kai Windows 3.1 + MELCO WSN-A4F 環境
