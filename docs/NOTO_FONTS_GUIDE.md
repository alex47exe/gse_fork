# Noto Fonts Guide for Complete Language and Emoji Support

## Important Note

**For Steam Languages:** If you specifically want to support all 30 Steam-supported languages plus emojis, see the focused guide:
- **[Noto Fonts for ALL Steam Languages + Emojis](NOTO_FONTS_FOR_STEAM.md)** ⭐

This document provides general Noto font information. The Steam-specific guide tells you exactly which 3 font files you need.

## Overview

This guide explains which **Noto font files** you need to use for comprehensive language and emoji support in the Steam overlay. Noto is Google's open-source font family designed to support all Unicode languages ("No Tofu" - no more □ boxes).

## Quick Answer

### Minimal Setup (Recommended for Overlay)

For the best balance of coverage and performance, use **one primary font file**:

**Option 1: Noto Sans CJK + Noto Color Emoji**
- Covers Latin, Chinese, Japanese, Korean, Cyrillic, Greek, and common symbols
- File: `NotoSansCJK-Regular.ttc` (~30-50MB depending on variant)
- Plus: `NotoColorEmoji.ttf` (~10MB) for emoji

**Option 2: Noto Sans + Noto Color Emoji**
- Covers Latin, Cyrillic, Greek, and common symbols
- File: `NotoSans-Regular.ttf` (~500KB)
- Plus: `NotoColorEmoji.ttf` (~10MB) for emoji
- More lightweight but no CJK support

### Full Coverage Setup (For Maximum Language Support)

For complete Unicode coverage across all languages, you would need **multiple Noto font files**, but this is impractical for the overlay due to memory constraints.

## Understanding Noto Font Family

### What is Noto?

- **Goal:** "No Tofu" - eliminate □ boxes for missing characters
- **Coverage:** ~1,000 languages, 162 writing systems
- **License:** Open source (SIL Open Font License)
- **Maintainer:** Google, in collaboration with Adobe (for CJK)

### Font Organization

Noto fonts are organized by **script** (writing system), not by language:

```
Noto Sans          → Latin, Cyrillic, Greek (base)
Noto Sans CJK      → Chinese, Japanese, Korean
Noto Sans Arabic   → Arabic script
Noto Sans Thai     → Thai script
Noto Sans Hebrew   → Hebrew script
Noto Color Emoji   → All emoji
... and 150+ more script-specific fonts
```

## Detailed Font Requirements

### 1. Core Latin and Common Scripts

**File:** `NotoSans-Regular.ttf`
- **Size:** ~500 KB
- **Covers:** 
  - Latin (English, French, German, Spanish, etc.)
  - Cyrillic (Russian, Ukrainian, Bulgarian)
  - Greek
  - Basic punctuation and symbols
- **Languages:** ~100+ Western/European languages
- **Download:** https://fonts.google.com/noto/specimen/Noto+Sans

### 2. CJK (Chinese, Japanese, Korean)

**File:** `NotoSansCJK-Regular.ttc` (or region-specific variants)
- **Size:** 30-120 MB (depending on variant and regional subset)
- **Covers:**
  - Simplified Chinese (SC)
  - Traditional Chinese (TC)
  - Japanese (JP) - Kanji, Hiragana, Katakana
  - Korean (KR) - Hangul
  - Shared CJK ideographs
- **Languages:** Chinese, Japanese, Korean
- **Download:** https://github.com/notofonts/noto-cjk/releases

**Regional Variants:**
- `NotoSansCJK-SC-Regular.otf` - Simplified Chinese (~25MB)
- `NotoSansCJK-TC-Regular.otf` - Traditional Chinese (~25MB)
- `NotoSansCJK-JP-Regular.otf` - Japanese (~25MB)
- `NotoSansCJK-KR-Regular.otf` - Korean (~25MB)

### 3. Emoji Support

**File:** `NotoColorEmoji.ttf`
- **Size:** ~10 MB
- **Covers:** All Unicode emoji (Emoji 15.1+)
- **Type:** Color emoji font (CBDT/CBLC tables)
- **Includes:**
  - Emoticons (😀 😊 👍)
  - Objects (🎨 🏆 🎮)
  - Nature (🌍 🦄 🌸)
  - Symbols (⭐ ❤️ ✓)
  - Flags (🇺🇸 🇯🇵 🇬🇧)
- **Download:** https://github.com/googlefonts/noto-emoji/releases

**Alternative:**
- `Noto-Emoji.ttf` - Black & white emoji (~3MB, lighter weight)

### 4. Additional Scripts (Optional)

If you need specific script coverage beyond Latin and CJK:

| Script | File | Size | Languages |
|--------|------|------|-----------|
| **Arabic** | NotoSansArabic-Regular.ttf | ~200 KB | Arabic, Urdu, Persian |
| **Thai** | NotoSansThai-Regular.ttf | ~50 KB | Thai |
| **Hebrew** | NotoSansHebrew-Regular.ttf | ~70 KB | Hebrew |
| **Devanagari** | NotoSansDevanagari-Regular.ttf | ~180 KB | Hindi, Sanskrit, Marathi |
| **Vietnamese** | Uses Noto Sans (already included) | - | Vietnamese |

**Full list:** https://fonts.google.com/noto/fonts

## Recommended Configurations

### Configuration 1: Lightweight (Best for Overlay)

**Fonts needed:**
1. `NotoSans-Regular.ttf` (~500 KB)
2. `NotoColorEmoji.ttf` (~10 MB)

**Coverage:**
- ✅ Latin languages (English, French, German, Spanish, etc.)
- ✅ Cyrillic (Russian, Ukrainian, Bulgarian)
- ✅ Greek
- ✅ Full emoji
- ❌ CJK (Chinese, Japanese, Korean)
- ❌ Arabic, Hebrew, Thai, etc.

**Total size:** ~10.5 MB

**Configuration:**
```ini
# overlay_appearance.txt
font_override=/path/to/NotoSans-Regular.ttf
# Note: ImGui doesn't support multiple fonts easily
# Emoji may need to be merged or in separate font
```

### Configuration 2: CJK Support (Comprehensive)

**Fonts needed:**
1. `NotoSansCJK-Regular.ttc` (~30-50 MB) OR regional variant
2. `NotoColorEmoji.ttf` (~10 MB)

**Coverage:**
- ✅ Latin languages
- ✅ Cyrillic, Greek
- ✅ Chinese (Simplified & Traditional)
- ✅ Japanese
- ✅ Korean
- ✅ Full emoji
- ❌ Arabic, Hebrew, Thai (unless separate fonts added)

**Total size:** ~40-60 MB

**Configuration:**
```ini
# overlay_appearance.txt
font_override=/path/to/NotoSansCJK-Regular.ttc
```

### Configuration 3: Maximum Coverage (Impractical)

For truly comprehensive coverage of all 162 scripts, you would need **100+ font files** totaling **500+ MB**. This is **NOT recommended** for the overlay due to:
- Memory consumption
- Loading time
- ImGui limitations (single font per atlas)

## Installation Guide

### Windows

1. **Download fonts:**
   - Noto Sans: https://fonts.google.com/noto/specimen/Noto+Sans
   - Noto Sans CJK: https://github.com/notofonts/noto-cjk/releases
   - Noto Color Emoji: https://github.com/googlefonts/noto-emoji/releases

2. **Install to Windows:**
   - Right-click font file → Install
   - Or copy to `C:\Windows\Fonts\`

3. **Configure overlay:**
   ```ini
   # Create/edit overlay_appearance.txt in steam_settings folder
   font_override=C:/Windows/Fonts/NotoSans-Regular.ttf
   # OR for CJK
   font_override=C:/Windows/Fonts/NotoSansCJK-Regular.ttc
   ```

### Linux

1. **Install via package manager:**
   ```bash
   # Ubuntu/Debian
   sudo apt install fonts-noto-core fonts-noto-cjk fonts-noto-color-emoji
   
   # Fedora
   sudo dnf install google-noto-sans-fonts google-noto-cjk-fonts google-noto-emoji-fonts
   
   # Arch
   sudo pacman -S noto-fonts noto-fonts-cjk noto-fonts-emoji
   ```

2. **Configure overlay:**
   ```ini
   # overlay_appearance.txt
   font_override=/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf
   # OR for CJK
   font_override=/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc
   ```

### macOS

1. **Download fonts** (same links as Windows)

2. **Install:**
   - Double-click font file → Install Font
   - Or copy to `~/Library/Fonts/`

3. **Configure overlay:**
   ```ini
   # overlay_appearance.txt
   font_override=/Library/Fonts/NotoSans-Regular.ttf
   # OR
   font_override=~/Library/Fonts/NotoSansCJK-Regular.ttc
   ```

## Font Merging Strategy

### The Challenge

ImGui overlay currently supports **one primary font** with the `font_override` setting. For multiple scripts (e.g., Latin + CJK + Emoji), you have three options:

### Option 1: Use Comprehensive CJK Font

**Best for:** Most users who need CJK support
- Use `NotoSansCJK-Regular.ttc` as primary font
- Includes: Latin, Cyrillic, Greek, CJK
- Missing: Emoji (shows as □)

### Option 2: Use Noto Sans + Unifont Fallback

**Best for:** Western languages only
- Use `NotoSans-Regular.ttf` as primary font
- Built-in Unifont provides fallback for missing glyphs
- Emoji may be limited

### Option 3: Custom Font Merging (Advanced)

If you need both CJK and Emoji, you would need to:
1. Modify overlay code to load multiple fonts
2. Configure ImGui to merge fonts
3. This requires code changes (not just configuration)

**Code changes required in `steam_overlay.cpp`:**
```cpp
// Load primary font
ImFont* font_primary = fonts_atlas.AddFontFromFileTTF("NotoSans.ttf", font_size, &font_cfg);

// Merge CJK font
font_cfg.MergeMode = true;
fonts_atlas.AddFontFromFileTTF("NotoSansCJK.ttc", font_size, &font_cfg);

// Merge emoji font
fonts_atlas.AddFontFromFileTTF("NotoColorEmoji.ttf", font_size, &font_cfg);
```

## File Size Considerations

### Memory Impact

| Font Configuration | Atlas Size | VRAM Usage | Languages Covered |
|-------------------|------------|------------|-------------------|
| Unifont (default) | ~2-4 MB | Baseline | All scripts (basic) |
| Noto Sans | ~3-5 MB | +1 MB | Latin, Cyrillic, Greek |
| Noto Sans + Emoji | ~12-15 MB | +10 MB | Latin + Emoji |
| Noto Sans CJK | ~35-40 MB | +30 MB | Latin + CJK |
| Noto Sans CJK + Emoji | ~45-50 MB | +40 MB | Latin + CJK + Emoji |

### Performance Notes

- **Loading time:** Large fonts (CJK) add 1-2 seconds to overlay initialization
- **Memory:** Font atlas is loaded into VRAM once
- **Runtime:** No performance impact after loading
- **Recommendation:** Use smallest font set that meets your needs

## Troubleshooting

### Problem: Font file too large, overlay slow to load

**Solution:** Use regional CJK variant instead of full CJK:
- Instead of `NotoSansCJK-Regular.ttc` (50MB)
- Use `NotoSansCJK-JP-Regular.otf` (25MB) for Japanese only
- Or `NotoSansCJK-SC-Regular.otf` (25MB) for Simplified Chinese only

### Problem: Emoji don't show with Noto Sans

**Cause:** Noto Sans doesn't include emoji glyphs

**Solution:** 
1. Use `NotoColorEmoji.ttf` instead (but you'll lose CJK)
2. Or modify code to merge multiple fonts (requires development)

### Problem: CJK characters show but emoji don't

**Cause:** CJK fonts don't include emoji

**Solution:** Same as above - need font merging (code change required)

### Problem: Want both CJK and Emoji

**Current limitation:** Overlay's `font_override` supports single font only

**Workarounds:**
1. Prioritize what you need most (CJK vs Emoji)
2. Use CJK font for text, accept missing emoji
3. Or modify overlay code to support font merging

## Download Links

### Official Sources

- **Google Fonts (Noto):** https://fonts.google.com/noto
- **Noto CJK GitHub:** https://github.com/notofonts/noto-cjk
- **Noto Emoji GitHub:** https://github.com/googlefonts/noto-emoji
- **All Noto Fonts:** https://notofonts.github.io/

### Direct Downloads (Latest Releases)

- **Noto Sans:** [Download from Google Fonts](https://fonts.google.com/noto/specimen/Noto+Sans)
- **Noto Sans CJK:** [Release Page](https://github.com/notofonts/noto-cjk/releases/latest)
- **Noto Color Emoji:** [Release Page](https://github.com/googlefonts/noto-emoji/releases/latest)

## Summary

### Quick Recommendations

**For Western Languages + Emoji:**
```ini
font_override=/path/to/NotoSans-Regular.ttf
```
Size: ~500 KB | Coverage: Latin, Cyrillic, Greek

**For East Asian Languages (CJK):**
```ini
font_override=/path/to/NotoSansCJK-Regular.ttc
```
Size: ~30-50 MB | Coverage: Latin, Cyrillic, Greek, Chinese, Japanese, Korean

**For Emoji Only:**
```ini
font_override=/path/to/NotoColorEmoji.ttf
```
Size: ~10 MB | Coverage: All emoji (but no text!)

### The Reality

For **truly comprehensive** support (all languages + emoji in one configuration), you would need:
- Code modification to support font merging
- Multiple Noto font files loaded and merged
- ~50-60 MB total font atlas

Current overlay limitation: **One font at a time via `font_override`**

Choose the font that covers your primary use case!
