#include "pages/offline/offline_content_page.h"

#include <Arduino.h>
#include <SD_MMC.h>

#include <cstring>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/sd_card/sd_card.h"
#include "font/xiaozhi_font.h"
#include "ui/localization.h"

namespace {

using Kind = OfflineContentPage::Kind;
constexpr uint8_t MAX_ITEMS = 10;
constexpr int LIST_TOP = 72;
constexpr int ROW_HEIGHT = 31;
constexpr int ROW_GAP = 2;
constexpr int READER_X = 10;
constexpr int READER_Y = 86;
constexpr int READER_W = 220;
constexpr int READER_LINE_H = 25;
constexpr int READER_LINES = 11;
constexpr uint16_t MAX_PAGES = 128;

struct Item {
    int32_t id = 0;
    char title[64] = {};
    char detail[96] = {};
    char directory[64] = {};
};

Kind activeKind = Kind::Voice;
Item items[MAX_ITEMS] = {};
uint8_t itemCount = 0;
int selectedIndex = -1;
int lastSelectedIndex = -1;
String selectedContent;
uint32_t pageOffsets[MAX_PAGES + 1] = {};
uint16_t pageCount = 1;
uint16_t currentPage = 0;

const char *folderFor(Kind kind) {
    switch (kind) {
    case Kind::Music: return "/music";
    case Kind::Poem: return "/poems";
    default: return "/voice";
    }
}

const char *titleFor(Kind kind) {
    switch (kind) {
    case Kind::Music: return "OFFLINE MUSIC";
    case Kind::Poem: return "OFFLINE POEMS";
    default: return "OFFLINE VOICE";
    }
}

void pixel(uint8_t *frame, int x, int y) {
    if (x < 0 || x >= XingtaiEpd::WIDTH || y < 0 || y >= XingtaiEpd::HEIGHT) return;
    frame[static_cast<size_t>(y) * (XingtaiEpd::WIDTH / 8) + x / 8] |= 0x80U >> (x % 8);
}

void line(uint8_t *frame, int x1, int y1, int x2, int y2) {
    const int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    const int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        pixel(frame, x1, y1);
        if (x1 == x2 && y1 == y2) break;
        const int doubled = error * 2;
        if (doubled >= dy) { error += dy; x1 += sx; }
        if (doubled <= dx) { error += dx; y1 += sy; }
    }
}

void rect(uint8_t *frame, int x, int y, int width, int height) {
    line(frame, x, y, x + width - 1, y);
    line(frame, x, y + height - 1, x + width - 1, y + height - 1);
    line(frame, x, y, x, y + height - 1);
    line(frame, x + width - 1, y, x + width - 1, y + height - 1);
}

void boldRect(uint8_t *frame, int x, int y, int width, int height) {
    rect(frame, x, y, width, height);
    if (width > 2 && height > 2) rect(frame, x + 1, y + 1, width - 2, height - 2);
}

bool inRect(int x, int y, int left, int top, int width, int height) {
    return x >= left && x < left + width && y >= top && y < top + height;
}

bool nextCodepoint(const char *&cursor, uint32_t &codepoint) {
    const uint8_t *s = reinterpret_cast<const uint8_t *>(cursor);
    if (!s || s[0] == 0) return false;
    if (s[0] < 0x80) { codepoint = s[0]; cursor += 1; return true; }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        codepoint = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); cursor += 2; return true;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        codepoint = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        cursor += 3; return true;
    }
    codepoint = '?'; ++cursor; return true;
}

void drawChineseGlyph(uint8_t *frame, const XiaozhiFont::Glyph *glyph, int x, int y,
                      int clipRight, int clipBottom) {
    if (!glyph) return;
    for (uint16_t gy = 0; gy < glyph->height && y + gy < clipBottom; ++gy) {
        for (uint16_t gx = 0; gx < glyph->width && x + gx < clipRight; ++gx) {
            const uint32_t index = static_cast<uint32_t>(gy) * glyph->width + gx;
            const uint8_t packed = glyph->bitmap[index / 2U];
            const uint8_t alpha = (index & 1U) ? (packed & 0x0FU) : (packed >> 4);
            if (alpha >= 11) pixel(frame, x + gx, y + gy);
        }
    }
}

int advanceFor(uint32_t codepoint) {
    if (codepoint < 0x80) return codepoint == ' ' ? 5 : 7;
    const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
    return glyph ? max(1, static_cast<int>(glyph->advance)) + 1 : 18;
}

void drawUtf8Line(uint8_t *frame, int x, int y, int width, int height, const char *text) {
    const char *cursor = text;
    const int right = x + width;
    while (cursor && *cursor && x < right) {
        uint32_t codepoint = 0;
        if (!nextCodepoint(cursor, codepoint)) break;
        const int advance = advanceFor(codepoint);
        if (x + advance > right) break;
        if (codepoint < 0x80) {
            char glyph[2] = {static_cast<char>(codepoint), 0};
            UiLocalization::drawText(frame, x, y + (height - 7) / 2, glyph);
        } else {
            const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(codepoint);
            if (glyph) {
                constexpr int baseline = 6;
                const int top = y + height - baseline - glyph->height - glyph->offsetY;
                drawChineseGlyph(frame, glyph, x + glyph->offsetX, top, right, y + height);
            }
        }
        x += advance;
    }
}

String readTrimmedLine(File &file) {
    String value = file.readStringUntil('\n');
    value.trim();
    return value;
}

bool loadItemMetadata(const char *directory, Kind kind, Item &item) {
    char path[96] = {};
    snprintf(path, sizeof(path), "%s/meta.txt", directory);
    File meta = SD_MMC.open(path, FILE_READ);
    if (!meta) return false;
    item.id = readTrimmedLine(meta).toInt();
    const String title = readTrimmedLine(meta);
    const String second = readTrimmedLine(meta);
    const String third = readTrimmedLine(meta);
    const String fourth = readTrimmedLine(meta);
    meta.close();
    snprintf(item.title, sizeof(item.title), "%s", title.length() ? title.c_str() : "UNTITLED");
    if (kind == Kind::Music) {
        snprintf(item.detail, sizeof(item.detail), "%s%s%s", third.c_str(),
                 third.length() && second.length() ? " / " : "", second.c_str());
        char audioPath[128] = {};
        snprintf(audioPath, sizeof(audioPath), "%s/%s", directory, second.c_str());
        if (!second.length() || !SD_MMC.exists(audioPath)) return false;
    } else {
        snprintf(item.detail, sizeof(item.detail), "%s%s%s", second.c_str(),
                 second.length() && third.length() ? " / " : "", third.c_str());
        snprintf(path, sizeof(path), "%s/content.txt", directory);
        if (!SD_MMC.exists(path)) return false;
    }
    snprintf(item.directory, sizeof(item.directory), "%s", directory);
    return true;
}

void loadItems(Kind kind) {
    itemCount = 0;
    selectedIndex = -1;
    lastSelectedIndex = -1;
    selectedContent = "";
    if (!SdCard::isMounted()) return;
    File root = SD_MMC.open(folderFor(kind));
    if (!root || !root.isDirectory()) return;
    File entry = root.openNextFile();
    while (entry && itemCount < MAX_ITEMS) {
        if (entry.isDirectory()) {
            String directory = entry.name();
            if (!directory.startsWith("/")) directory = String(folderFor(kind)) + "/" + directory;
            Item candidate;
            if (loadItemMetadata(directory.c_str(), kind, candidate)) items[itemCount++] = candidate;
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    Serial.printf("[OFFLINE] kind=%u folder=%s items=%u\n",
                  static_cast<unsigned>(kind), folderFor(kind), itemCount);
}

void paginateContent() {
    pageCount = 1;
    currentPage = 0;
    pageOffsets[0] = 0;
    const char *base = selectedContent.c_str();
    const char *cursor = base;
    int lineIndex = 0;
    int lineWidth = 0;
    while (*cursor) {
        const char *start = cursor;
        uint32_t codepoint = 0;
        nextCodepoint(cursor, codepoint);
        if (codepoint == '\r') continue;
        if (codepoint == '\n') {
            lineWidth = 0;
            if (++lineIndex >= READER_LINES && pageCount < MAX_PAGES) {
                pageOffsets[pageCount++] = cursor - base;
                lineIndex = 0;
            }
            continue;
        }
        const int advance = advanceFor(codepoint);
        if (lineWidth > 0 && lineWidth + advance > READER_W) {
            lineWidth = 0;
            if (++lineIndex >= READER_LINES && pageCount < MAX_PAGES) {
                pageOffsets[pageCount++] = start - base;
                lineIndex = 0;
            }
        }
        lineWidth += advance;
    }
    pageOffsets[pageCount] = selectedContent.length();
}

void selectItem(uint8_t index) {
    if (index >= itemCount) return;
    selectedIndex = index;
    if (activeKind == Kind::Music || activeKind == Kind::Poem) {
        lastSelectedIndex = index;
    }
    currentPage = 0;
    const Item &item = items[index];
    if (activeKind == Kind::Voice || activeKind == Kind::Poem) {
        char path[96] = {};
        snprintf(path, sizeof(path), "%s/content.txt", item.directory);
        File content = SD_MMC.open(path, FILE_READ);
        selectedContent = content ? content.readString() : String();
        if (content) content.close();
        paginateContent();
    }
}

void renderList(uint8_t *frame) {
    UiLocalization::drawCentered(frame, 42, titleFor(activeKind));
    if (!SdCard::isMounted()) {
        UiLocalization::drawCentered(frame, 190, "SD CARD NOT READY");
        return;
    }
    if (itemCount == 0) {
        UiLocalization::drawCentered(frame, 180, "NO SAVED CONTENT");
        UiLocalization::drawCentered(frame, 205, folderFor(activeKind));
        return;
    }
    for (uint8_t i = 0; i < itemCount; ++i) {
        const int top = LIST_TOP + i * (ROW_HEIGHT + ROW_GAP);
        if (i == lastSelectedIndex &&
            (activeKind == Kind::Music || activeKind == Kind::Poem)) {
            boldRect(frame, 10, top, 220, ROW_HEIGHT);
        } else {
            rect(frame, 10, top, 220, ROW_HEIGHT);
        }
        drawUtf8Line(frame, 17, top + 2, 188, ROW_HEIGHT - 4, items[i].title);
        line(frame, 214, top + 8, 222, top + 15);
        line(frame, 222, top + 15, 214, top + 22);
    }
}

void renderTextReader(uint8_t *frame) {
    const Item &item = items[selectedIndex];
    rect(frame, 8, 38, 48, 25);
    UiLocalization::drawText(frame, 16, 47, "BACK");
    drawUtf8Line(frame, 64, 38, 166, 25, item.title);
    line(frame, 8, 70, 231, 70);
    const char *base = selectedContent.c_str();
    const char *cursor = base + pageOffsets[currentPage];
    const uint32_t end = pageOffsets[currentPage + 1];
    int x = READER_X, lineIndex = 0;
    while (*cursor && static_cast<uint32_t>(cursor - base) < end && lineIndex < READER_LINES) {
        uint32_t cp = 0;
        nextCodepoint(cursor, cp);
        if (cp == '\r') continue;
        if (cp == '\n') { x = READER_X; ++lineIndex; continue; }
        const int advance = advanceFor(cp);
        if (x > READER_X && x + advance > READER_X + READER_W) {
            x = READER_X;
            if (++lineIndex >= READER_LINES) break;
        }
        const int top = READER_Y + lineIndex * READER_LINE_H;
        if (cp < 0x80) {
            char text[2] = {static_cast<char>(cp), 0};
            UiLocalization::drawText(frame, x, top + 9, text);
        } else {
            const XiaozhiFont::Glyph *glyph = XiaozhiFont::glyph(cp);
            if (glyph) drawChineseGlyph(frame, glyph, x + glyph->offsetX,
                                        top + READER_LINE_H - 6 - glyph->height - glyph->offsetY,
                                        READER_X + READER_W, top + READER_LINE_H);
        }
        x += advance;
    }
    rect(frame, 8, 386, 34, 24);
    line(frame, 29, 391, 20, 398); line(frame, 20, 398, 29, 405);
    rect(frame, 198, 386, 34, 24);
    line(frame, 211, 391, 220, 398); line(frame, 220, 398, 211, 405);
    char pages[20] = {};
    snprintf(pages, sizeof(pages), "%u/%u", currentPage + 1, pageCount);
    UiLocalization::drawCentered(frame, 394, pages);
}

void renderMusicDetail(uint8_t *frame) {
    const Item &item = items[selectedIndex];
    rect(frame, 8, 38, 48, 25); UiLocalization::drawText(frame, 16, 47, "BACK");
    drawUtf8Line(frame, 20, 92, 200, 34, item.title);
    UiLocalization::drawCentered(frame, 150, "SAVED ON SD");
    drawUtf8Line(frame, 20, 185, 200, 28, item.detail);
    UiLocalization::drawCentered(frame, 250, "AUDIO FILE READY");
    UiLocalization::drawCentered(frame, 275, "DECODER NOT INSTALLED");
}

}

namespace OfflineContentPage {

void open(Kind kind) {
    activeKind = kind;
    loadItems(kind);
}

bool handleTap(Kind kind, int16_t x, int16_t y) {
    if (kind != activeKind) open(kind);
    if (selectedIndex < 0) {
        for (uint8_t i = 0; i < itemCount; ++i) {
            const int top = LIST_TOP + i * (ROW_HEIGHT + ROW_GAP);
            if (inRect(x, y, 10, top, 220, ROW_HEIGHT)) {
                selectItem(i);
                return true;
            }
        }
        return false;
    }
    if (inRect(x, y, 8, 38, 48, 25)) { selectedIndex = -1; return true; }
    if ((kind == Kind::Voice || kind == Kind::Poem) &&
        inRect(x, y, 8, 386, 34, 24) && currentPage > 0) {
        --currentPage; return true;
    }
    if ((kind == Kind::Voice || kind == Kind::Poem) &&
        inRect(x, y, 198, 386, 34, 24) && currentPage + 1 < pageCount) {
        ++currentPage; return true;
    }
    return false;
}

void render(Kind kind, uint8_t *frame) {
    if (kind != activeKind) open(kind);
    std::memset(frame, 0x00, XingtaiEpd::FRAME_BYTES);
    if (selectedIndex < 0) renderList(frame);
    else if (kind == Kind::Voice || kind == Kind::Poem) renderTextReader(frame);
    else renderMusicDetail(frame);
}

}