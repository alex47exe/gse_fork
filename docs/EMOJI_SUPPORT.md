# Emoji Support in Steam Overlay

## Overview

The Steam overlay now includes support for Windows 11 25H2 Emoji 16.0 Unicode ranges. This document explains how emoji rendering works and how to configure it for optimal results.

## Do I Need a Special Font for Emojis?

**YES** - Emoji rendering requires a font that contains emoji glyphs. The overlay supports emoji through its font loading system, but the actual rendering depends on which font is loaded.

## How Emoji Display Works

### Default Configuration (Unifont)

By default, the overlay uses **Unifont**, a built-in compressed font designed for broad Unicode coverage:

- **Basic symbols** (☀️ ⭐ ✈️ ✓ ✂️): May render as simple monochrome glyphs
- **Complex emoji** (😀 🎨 🦄 🚀): Will likely display as □ (missing glyph boxes)
- Unifont prioritizes small size and wide character coverage over complete emoji support

### With Custom Emoji Font

For full emoji support, configure a custom emoji font using the `font_override` setting:

1. **Full emoji rendering** with all modern emoji glyphs
2. **Proper display** of emoji from text files, chat, usernames, etc.
3. **Monochrome rendering** (standard ImGui limitation)

## Configuration Guide

### Windows
```ini
font_override=C:/Windows/Fonts/seguiemj.ttf
```

### Linux
```ini
font_override=/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf
```

### macOS
```ini
font_override=/System/Library/Fonts/Apple Color Emoji.ttc
```

Add this to `overlay_appearance.txt` in your steam_settings folder.

## Emoji in Text Files

When the overlay reads text from files (chat logs, usernames, configuration files, etc.):

1. **Text is decoded** as UTF-8
2. **Emoji characters** are recognized by their Unicode codepoints
3. **Font atlas lookup** checks if the loaded font has the emoji glyph
4. **Rendering:**
   - ✅ If glyph exists: Emoji is rendered
   - ❌ If glyph missing: □ box is displayed

### Example Scenarios

**Username with emoji:**
```
File content: "Player🎮123"
Default font: "Player□123"
Emoji font:   "Player🎮123"
```

**Chat message:**
```
File content: "Good game! 😊👍"
Default font: "Good game! □□"
Emoji font:   "Good game! 😊👍"
```

## Noto Fonts for Emoji

For comprehensive emoji support using open-source fonts, see:
- **[Noto Fonts Guide](NOTO_FONTS_GUIDE.md)** - Complete guide for Noto fonts
- **Noto Color Emoji** - Recommended: `NotoColorEmoji.ttf` (~10 MB)
- **Download:** https://github.com/googlefonts/noto-emoji/releases

## Summary

- ✅ **Emoji Unicode ranges**: Included by default
- ✅ **Basic symbols**: Work with Unifont
- ⚠️ **Complex emoji**: Need custom emoji font
- ⚠️ **Configuration required**: Set `font_override` for full support
- ❌ **Color emoji**: Not supported (monochrome only)
- ✅ **Text files**: Emoji from files render if font supports them

**For the best emoji experience, configure an emoji font using the `font_override` setting!**

### Recommended Fonts

- **Windows:** Segoe UI Emoji (`seguiemj.ttf`)
- **Linux/Cross-platform:** Noto Color Emoji (`NotoColorEmoji.ttf`)
- **macOS:** Apple Color Emoji (`Apple Color Emoji.ttc`)

See [Noto Fonts Guide](NOTO_FONTS_GUIDE.md) for comprehensive language and emoji support options.
