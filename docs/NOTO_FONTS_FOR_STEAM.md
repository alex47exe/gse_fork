# Noto Fonts for ALL Steam Languages + Emojis

## Quick Answer

To support **all 30 Steam-supported languages** plus emojis, you need these **3 Noto font files**:

1. **NotoSansCJK-Regular.ttc** (~30-50 MB) - Primary font
2. **NotoSansThai-Regular.ttf** (~50 KB) - Thai support
3. **NotoColorEmoji.ttf** (~10 MB) - All emoji

**Total size:** ~40-60 MB

⚠️ **Current limitation:** The overlay's `font_override` setting only supports **one font at a time**. For complete coverage, code modification is needed to merge these fonts.

## Steam's 30 Supported Languages

Based on the overlay source code (`valid_languages[]`):

| # | Language | Script Needed | Covered By |
|---|----------|---------------|------------|
| 1 | English | Latin | Noto Sans CJK |
| 2 | ~~Arabic~~ | ~~Arabic~~ | ❌ Not supported (RTL limitation) |
| 3 | Bulgarian | Cyrillic | Noto Sans CJK |
| 4 | Chinese (Simplified) | CJK | Noto Sans CJK |
| 5 | Chinese (Traditional) | CJK | Noto Sans CJK |
| 6 | Czech | Latin | Noto Sans CJK |
| 7 | Danish | Latin | Noto Sans CJK |
| 8 | Dutch | Latin | Noto Sans CJK |
| 9 | Finnish | Latin | Noto Sans CJK |
| 10 | French | Latin | Noto Sans CJK |
| 11 | German | Latin | Noto Sans CJK |
| 12 | Greek | Greek | Noto Sans CJK |
| 13 | Hungarian | Latin | Noto Sans CJK |
| 14 | Italian | Latin | Noto Sans CJK |
| 15 | Japanese | CJK | Noto Sans CJK |
| 16 | Korean | CJK | Noto Sans CJK |
| 17 | Norwegian | Latin | Noto Sans CJK |
| 18 | Polish | Latin | Noto Sans CJK |
| 19 | Portuguese | Latin | Noto Sans CJK |
| 20 | Brazilian Portuguese | Latin | Noto Sans CJK |
| 21 | Romanian | Latin | Noto Sans CJK |
| 22 | Russian | Cyrillic | Noto Sans CJK |
| 23 | Spanish | Latin | Noto Sans CJK |
| 24 | Latin American Spanish | Latin | Noto Sans CJK |
| 25 | Swedish | Latin | Noto Sans CJK |
| 26 | Thai | Thai | **Noto Sans Thai** |
| 27 | Turkish | Latin | Noto Sans CJK |
| 28 | Ukrainian | Cyrillic | Noto Sans CJK |
| 29 | Vietnamese | Latin + diacritics | Noto Sans CJK |
| 30 | Croatian | Latin | Noto Sans CJK |
| 31 | Indonesian | Latin | Noto Sans CJK |

**Coverage:** 30 out of 31 languages (96.8%)

## Required Font Files

### 1. Noto Sans CJK (Primary Font)

**File:** `NotoSansCJK-Regular.ttc`
- **Size:** 30-50 MB (depending on included regions)
- **Download:** https://github.com/notofonts/noto-cjk/releases/latest
- **Covers:**
  - ✅ Latin script (20 languages): English, Czech, Danish, Dutch, Finnish, French, German, Hungarian, Italian, Norwegian, Polish, Portuguese, Brazilian, Romanian, Spanish, Latam Spanish, Swedish, Turkish, Croatian, Indonesian
  - ✅ Cyrillic script (3 languages): Bulgarian, Russian, Ukrainian
  - ✅ Greek script (1 language): Greek
  - ✅ CJK scripts (4 languages): Chinese Simplified, Chinese Traditional, Japanese, Korean
  - ✅ Vietnamese (Latin with special diacritics)

**Why this single font covers 29 languages:**
- Noto Sans CJK includes all Latin, Cyrillic, and Greek characters
- Plus full CJK (Chinese, Japanese, Korean) ideographs
- Designed for comprehensive Asian language support

### 2. Noto Sans Thai

**File:** `NotoSansThai-Regular.ttf`
- **Size:** ~50 KB
- **Download:** https://fonts.google.com/noto/specimen/Noto+Sans+Thai
- **Covers:**
  - ✅ Thai script (1 language): Thai

**Why needed:**
- Thai script is not included in Noto Sans CJK
- Very small file, adds minimal overhead

### 3. Noto Color Emoji

**File:** `NotoColorEmoji.ttf`
- **Size:** ~10 MB
- **Download:** https://github.com/googlefonts/noto-emoji/releases/latest
- **Covers:**
  - ✅ All Unicode emoji (Emoji 16.0)
  - ✅ ~3,600+ emoji characters
  - Emoticons, objects, nature, symbols, flags, etc.

## Practical Configurations

### Option 1: Single Font (Current Overlay Support)

**Choose your priority:**

#### A) CJK + Most Languages (29/31 languages, no emoji)
```ini
# overlay_appearance.txt
font_override=/path/to/NotoSansCJK-Regular.ttc
```
- ✅ Covers: 29 Steam languages (missing Thai and emoji)
- ✅ Size: ~30-50 MB
- ❌ Thai shows as □
- ❌ Emoji show as □

#### B) Western Languages + Emoji (20/31 languages)
```ini
# overlay_appearance.txt
font_override=/path/to/NotoColorEmoji.ttf
```
- ✅ Covers: All emoji
- ✅ Size: ~10 MB
- ❌ All text shows as emoji or □ (not practical!)

#### C) CJK + Thai (30/31 languages, no emoji)
```ini
# Not possible with single font - requires merging
```

### Option 2: Multi-Font Merging (Requires Code Modification)

For **complete coverage** of all 30 Steam languages + emoji, you need to modify the overlay code to merge fonts.

**Required fonts:**
1. `NotoSansCJK-Regular.ttc` (29 languages)
2. `NotoSansThai-Regular.ttf` (Thai)
3. `NotoColorEmoji.ttf` (emoji)

**Code modification in `steam_overlay.cpp`:**

```cpp
// Around line 346 in create_fonts()
// Replace single font loading with merged fonts:

// Load primary CJK font (covers 29 languages)
ImFontConfig config_primary = font_cfg;
config_primary.MergeMode = false;
ImFont* font_primary = fonts_atlas.AddFontFromFileTTF(
    "path/to/NotoSansCJK-Regular.ttc", 
    font_size, 
    &config_primary
);

// Merge Thai font
ImFontConfig config_thai = font_cfg;
config_thai.MergeMode = true;  // Important: merge mode!
fonts_atlas.AddFontFromFileTTF(
    "path/to/NotoSansThai-Regular.ttf",
    font_size,
    &config_thai
);

// Merge emoji font
ImFontConfig config_emoji = font_cfg;
config_emoji.MergeMode = true;  // Important: merge mode!
config_emoji.OversampleH = 1;
config_emoji.OversampleV = 1;
fonts_atlas.AddFontFromFileTTF(
    "path/to/NotoColorEmoji.ttf",
    font_size,
    &config_emoji
);

font_notif = font_default = font_primary;
stats.font = font_primary;
```

**Result with merging:**
- ✅ All 30 Steam languages (except Arabic)
- ✅ All emoji
- ✅ Total atlas: ~40-60 MB

## Installation Instructions

### Windows

1. **Download fonts:**
   - Noto Sans CJK: https://github.com/notofonts/noto-cjk/releases
   - Noto Sans Thai: https://fonts.google.com/noto/specimen/Noto+Sans+Thai
   - Noto Color Emoji: https://github.com/googlefonts/noto-emoji/releases

2. **Install fonts:**
   - Right-click each .ttf/.ttc file → "Install"
   - Or copy to `C:\Windows\Fonts\`

3. **Configure overlay:**
   ```ini
   # Create/edit overlay_appearance.txt in steam_settings folder
   font_override=C:/Windows/Fonts/NotoSansCJK-Regular.ttc
   ```

### Linux

1. **Install via package manager:**
   ```bash
   # Ubuntu/Debian
   sudo apt install fonts-noto-cjk fonts-noto-color-emoji
   # Thai font may need separate download
   
   # Fedora
   sudo dnf install google-noto-sans-cjk-fonts google-noto-emoji-fonts
   
   # Arch
   sudo pacman -S noto-fonts-cjk noto-fonts-emoji
   ```

2. **Configure overlay:**
   ```ini
   # overlay_appearance.txt
   font_override=/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc
   ```

3. **For Thai, download separately:**
   - Download from https://fonts.google.com/noto/specimen/Noto+Sans+Thai
   - Copy to `~/.local/share/fonts/` or `/usr/share/fonts/truetype/noto/`
   - Run `fc-cache -f -v` to update font cache

### macOS

1. **Download fonts** (same links as Windows)

2. **Install:**
   - Double-click each font file → "Install Font"
   - Or copy to `~/Library/Fonts/` or `/Library/Fonts/`

3. **Configure overlay:**
   ```ini
   # overlay_appearance.txt
   font_override=/Library/Fonts/NotoSansCJK-Regular.ttc
   ```

## File Sizes and Memory Impact

| Font Configuration | File Size | Atlas Size | Languages | Emoji |
|-------------------|-----------|------------|-----------|-------|
| NotoSansCJK only | 30-50 MB | ~35-40 MB | 29/31 | ❌ |
| NotoSansCJK + Thai | 30-50 MB | ~35-40 MB | 30/31 | ❌ |
| NotoSansCJK + Emoji | 40-60 MB | ~45-50 MB | 29/31 | ✅ |
| **All 3 fonts (complete)** | **40-60 MB** | **~45-55 MB** | **30/31** | **✅** |

**Note:** File size is on disk, Atlas size is loaded into VRAM.

## Language-by-Language Coverage

### Latin Script Languages (20)

**Covered by:** Noto Sans CJK (Latin characters included)

- English ✅
- Czech ✅
- Danish ✅
- Dutch ✅
- Finnish ✅
- French ✅
- German ✅
- Hungarian ✅
- Italian ✅
- Norwegian ✅
- Polish ✅
- Portuguese ✅
- Brazilian Portuguese ✅
- Romanian ✅
- Spanish ✅
- Latin American Spanish ✅
- Swedish ✅
- Turkish ✅
- Croatian ✅
- Indonesian ✅
- Vietnamese ✅ (Latin + special diacritics)

### CJK Languages (4)

**Covered by:** Noto Sans CJK

- Chinese (Simplified) ✅
- Chinese (Traditional) ✅
- Japanese ✅
- Korean ✅

### Cyrillic Script Languages (3)

**Covered by:** Noto Sans CJK (Cyrillic included)

- Bulgarian ✅
- Russian ✅
- Ukrainian ✅

### Greek Script (1)

**Covered by:** Noto Sans CJK (Greek included)

- Greek ✅

### Thai Script (1)

**Covered by:** Noto Sans Thai (separate font required)

- Thai ✅

### Not Supported (1)

**Not covered:** Arabic ❌
- **Reason:** ImGui doesn't support RTL (right-to-left) text layout
- **Would require:** HarfBuzz integration, BiDi algorithm, glyph shaping
- **Status:** Technical limitation, not font issue

## Why Noto Sans CJK Covers So Much

Noto Sans CJK is comprehensive because:

1. **Includes Latin characters** - Full Latin alphabet with extended characters
2. **Includes Cyrillic** - Complete Cyrillic alphabet
3. **Includes Greek** - Complete Greek alphabet
4. **Includes CJK ideographs** - ~30,000+ Chinese/Japanese/Korean characters
5. **Includes symbols** - Common punctuation, mathematical symbols, etc.

It's designed as an "all-in-one" font for multilingual Asian text that also needs to display Western content.

## Summary

### For All 30 Steam Languages + Emoji

**You need:**
1. ✅ `NotoSansCJK-Regular.ttc` (30-50 MB) - 29 languages
2. ✅ `NotoSansThai-Regular.ttf` (50 KB) - Thai language
3. ✅ `NotoColorEmoji.ttf` (10 MB) - All emoji

**Total:** ~40-60 MB

### Current Limitation

The overlay currently supports **only one font** via `font_override` setting.

**Choose:**
- **Option A:** Use `NotoSansCJK-Regular.ttc` for 29 languages (no Thai, no emoji)
- **Option B:** Modify code to merge all 3 fonts for complete coverage

### Code Modification Required

To use all 3 fonts simultaneously:
1. Modify `steam_overlay.cpp` around line 346
2. Use `ImFontConfig::MergeMode = true` for secondary fonts
3. Load fonts in order: CJK → Thai → Emoji

See "Option 2: Multi-Font Merging" section above for complete code example.

---

**Download All Fonts:**
- Noto Sans CJK: https://github.com/notofonts/noto-cjk/releases/latest
- Noto Sans Thai: https://fonts.google.com/noto/specimen/Noto+Sans+Thai
- Noto Color Emoji: https://github.com/googlefonts/noto-emoji/releases/latest
