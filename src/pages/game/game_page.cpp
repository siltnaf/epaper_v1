#include "pages/game/game_page.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <cstring>

#include "devices/epd_xingtai/epd_xingtai.h"
#include "devices/ml307/ml307.h"
#include "ui/loading_indicator.h"
#include "ui/localization.h"

namespace {
enum class Game : uint8_t { List, InternationalChess, ChineseChess, DouDizhu };
Game selectedGame = Game::List;
char contentUrl[128] = {};
char feedback[112] = "Choose a game";
bool exitRequested = false;
int8_t selectedX = -1;
int8_t selectedY = -1;
bool whiteToMove = true;
bool redToMove = true;
char chess[8][8] = {
    {'r','n','b','q','k','b','n','r'}, {'p','p','p','p','p','p','p','p'},
    {' ',' ',' ',' ',' ',' ',' ',' '}, {' ',' ',' ',' ',' ',' ',' ',' '},
    {' ',' ',' ',' ',' ',' ',' ',' '}, {' ',' ',' ',' ',' ',' ',' ',' '},
    {'P','P','P','P','P','P','P','P'}, {'R','N','B','Q','K','B','N','R'},
};
char xiangqi[10][9] = {
    {'k','a','b','n','r','n','b','a','k'}, {' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {' ','c',' ',' ',' ',' ',' ','c',' '}, {'p',' ','p',' ','p',' ','p',' ','p'},
    {' ',' ',' ',' ',' ',' ',' ',' ',' '}, {' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'P',' ','P',' ','P',' ','P',' ','P'}, {' ','C',' ',' ',' ',' ',' ','C',' '},
    {' ',' ',' ',' ',' ',' ',' ',' ',' '}, {'K','A','B','N','R','N','B','A','K'},
};

constexpr int BOARD_X = 8;
constexpr int BOARD_Y = 76;
constexpr int CHESS_CELL = 28;
constexpr int XIANGQI_CELL_X = 27;
constexpr int XIANGQI_CELL_Y = 36;
constexpr int XIANGQI_LEFT = 12;
constexpr int XIANGQI_TOP = 74;
constexpr int XIANGQI_PIECE_RADIUS = 11;
constexpr int RETURN_ICON_CX = 26;
constexpr int RETURN_ICON_CY = 54;
constexpr int NEW_GAME_ICON_CX = 214;
constexpr int NEW_GAME_ICON_CY = 54;
constexpr int GAME_ICON_RADIUS = 12;

void copyText(char *dst, size_t size, const char *src) { if (dst && size) { std::strncpy(dst, src ? src : "", size - 1); dst[size - 1] = 0; } }
void pixel(uint8_t *f, int x, int y) { if (x >= 0 && x < XingtaiEpd::WIDTH && y >= 0 && y < XingtaiEpd::HEIGHT) f[static_cast<size_t>(y) * 30 + x / 8] |= 0x80U >> (x % 8); }
void line(uint8_t *f, int x0, int y0, int x1, int y1) { int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, e=dx+dy; while(true){pixel(f,x0,y0);if(x0==x1&&y0==y1)break;int e2=2*e;if(e2>=dy){e+=dy;x0+=sx;}if(e2<=dx){e+=dx;y0+=sy;}} }
void rect(uint8_t *f,int x,int y,int w,int h){line(f,x,y,x+w-1,y);line(f,x,y+h-1,x+w-1,y+h-1);line(f,x,y,x,y+h-1);line(f,x+w-1,y,x+w-1,y+h-1);}
void clearRect(uint8_t *f,int x,int y,int w,int h){for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)if(xx>=0&&xx<XingtaiEpd::WIDTH&&yy>=0&&yy<XingtaiEpd::HEIGHT)f[static_cast<size_t>(yy)*30+xx/8]&=static_cast<uint8_t>(~(0x80U>>(xx%8)));}
bool inRect(int16_t x,int16_t y,int l,int t,int w,int h){return x>=l&&x<l+w&&y>=t&&y<t+h;}
String host() { String base(contentUrl); base.trim(); while(base.endsWith("/")) base.remove(base.length()-1); int scheme=base.indexOf("://"); if(scheme>=0){int path=base.indexOf('/',scheme+3);if(path>=0)base.remove(path);} return base; }
String macAddress() { String mac=WiFi.macAddress(); return mac == "00:00:00:00:00:00" ? String("ESP32-S3") : mac; }
const char *text(const char *english, const char *chinese) { return UiLocalization::isChinese() ? chinese : english; }

void resetBoard() {
    const char initialChess[8][8]={{'r','n','b','q','k','b','n','r'},{'p','p','p','p','p','p','p','p'},{' ',' ',' ',' ',' ',' ',' ',' '},{' ',' ',' ',' ',' ',' ',' ',' '},{' ',' ',' ',' ',' ',' ',' ',' '},{' ',' ',' ',' ',' ',' ',' ',' '},{'P','P','P','P','P','P','P','P'},{'R','N','B','Q','K','B','N','R'}};
    const char initialXiangqi[10][9]={{'k','a','b','n','r','n','b','a','k'},{' ',' ',' ',' ',' ',' ',' ',' ',' '},{' ','c',' ',' ',' ',' ',' ','c',' '},{'p',' ','p',' ','p',' ','p',' ','p'},{' ',' ',' ',' ',' ',' ',' ',' ',' '},{' ',' ',' ',' ',' ',' ',' ',' ',' '},{'P',' ','P',' ','P',' ','P',' ','P'},{' ','C',' ',' ',' ',' ',' ','C',' '},{' ',' ',' ',' ',' ',' ',' ',' ',' '},{'K','A','B','N','R','N','B','A','K'}};
    std::memcpy(chess,initialChess,sizeof(chess)); std::memcpy(xiangqi,initialXiangqi,sizeof(xiangqi));
    whiteToMove=true; redToMove=true; selectedX=selectedY=-1;
}
String chessState() { String out; for(int y=0;y<8;++y){int empty=0;for(int x=0;x<8;++x){if(chess[y][x]==' ')++empty;else{if(empty){out+=String(empty);empty=0;}out+=chess[y][x];}}if(empty)out+=String(empty);if(y<7)out+='/';} return out+(whiteToMove?" w KQkq - 0 1":" b KQkq - 0 1"); }
String xiangqiState() { String out; for(int y=0;y<10;++y){int empty=0;for(int x=0;x<9;++x){if(xiangqi[y][x]==' ')++empty;else{if(empty){out+=String(empty);empty=0;}out+=xiangqi[y][x];}}if(empty)out+=String(empty);if(y<9)out+='/';}return out+(redToMove?" w":" b"); }
bool applyChessMove(const String &move) {
    if (move.length() < 4) return false;
    const int fromX = move[0] - 'a';
    const int fromY = 8 - (move[1] - '0');
    const int toX = move[2] - 'a';
    const int toY = 8 - (move[3] - '0');
    if (fromX < 0 || fromX >= 8 || toX < 0 || toX >= 8 ||
        fromY < 0 || fromY >= 8 || toY < 0 || toY >= 8) return false;
    const char piece = chess[fromY][fromX];
    if (piece == ' ') return false;
    chess[toY][toX] = piece;
    chess[fromY][fromX] = ' ';
    return true;
}
bool applyChessFen(const String &fen) {
    int row = 0;
    int column = 0;
    for (size_t index = 0; index < fen.length() && row < 8; ++index) {
        const char value = fen[index];
        if (value == ' ') break;
        if (value == '/') { if (column != 8) return false; ++row; column = 0; continue; }
        if (value >= '1' && value <= '8') {
            const int count = value - '0';
            if (column + count > 8) return false;
            for (int offset = 0; offset < count; ++offset) chess[row][column++] = ' ';
        } else {
            if (column >= 8 || (value < 'A' || value > 'Z') && (value < 'a' || value > 'z')) return false;
            chess[row][column++] = value;
        }
    }
    return row == 7 && column == 8;
}
bool postMove(const String &move, bool hasMove) {
    if(WiFi.status()!=WL_CONNECTED){copyText(feedback,sizeof(feedback),text("WiFi required for AI","AI 需要网络"));return false;}
    JsonDocument request;
    if(selectedGame==Game::InternationalChess){request["game"]="international-chess";request["state"]=chessState();if(hasMove)request["move"]=move;}
    else if(selectedGame==Game::ChineseChess){request["game"]="chinese-chess";request["state"]=xiangqiState();request["side"]=redToMove?"RED":"BLACK";if(hasMove)request["move"]=move;}
    else {request["game"]="doudizhu";JsonObject state=request["state"].to<JsonObject>();state["hand"]="3 3 4 5 6 7 8 9 J Q K A 2";state["lastPlay"]="";}
    String body;serializeJson(request,body);
    HTTPClient http;WiFiClient plain;WiFiClientSecure secure;secure.setInsecure();String url=host()+"/api/board/move";
    bool began=url.startsWith("https://")?http.begin(secure,url):http.begin(plain,url);if(!began){copyText(feedback,sizeof(feedback),text("Invalid content URL","内容网址无效"));return false;}
    UiLoadingIndicator::Scope loading;http.setConnectTimeout(7000);http.setTimeout(20000);http.addHeader("Content-Type","application/json");http.addHeader("X-Device-MAC",macAddress());http.addHeader("Connection","close");
    int code=http.POST(body);String response=code>=200&&code<300?http.getString():String();http.end();
    if(code<200||code>=300){copyText(feedback,sizeof(feedback),text("AI request failed","AI 请求失败"));return false;}
    JsonDocument reply;if(deserializeJson(reply,response)){copyText(feedback,sizeof(feedback),text("Invalid engine response","引擎响应无效"));return false;}
    if (selectedGame == Game::InternationalChess && hasMove) {
        String aiMove = reply["aiMove"] | "";
        if (aiMove.isEmpty()) aiMove = reply["opponentMove"] | "";
        if (aiMove.isEmpty()) aiMove = reply["ai_move"] | "";
        if (aiMove.isEmpty()) aiMove = reply["opponent_move"] | "";
        if (aiMove.isEmpty()) aiMove = reply["move"] | "";
        bool applied = applyChessMove(aiMove);
        if (!applied) {
            const char *fen = reply["fen"] | reply["state"]["fen"] | "";
            if (fen[0] != '\0') applied = applyChessFen(fen);
        }
        if (applied) {
            whiteToMove = true;
            copyText(feedback, sizeof(feedback), text("AI moved","AI 已走棋"));
        } else {
            copyText(feedback, sizeof(feedback), text("AI move missing","未收到 AI 走法"));
            return false;
        }
    } else {
        copyText(feedback, sizeof(feedback), text("Move sent","已发送走法"));
    }
    return true;
}
const char *xiangqiSymbol(char piece) {
    switch (piece) {
    case 'r': return "\xE5\xB0\x87"; // bG.svg: 將
    case 'n': return "\xE5\xA3\xAB"; // bA.svg: 士
    case 'b': return "\xE8\xB1\xA1"; // bE.svg: 象
    case 'a': return "\xE9\xA6\xAC"; // bH.svg: 馬
    case 'k': return "\xE8\xBB\x8A"; // bR.svg: 車
    case 'c': return "\xE7\x82\xAE"; // bC.svg: 炮
    case 'p': return "\xE5\x8D\x92"; // bS.svg: 卒
    case 'R': return "\xE5\xB8\xA5"; // rG.svg: 帥
    case 'N': return "\xE4\xBB\x95"; // rA.svg: 仕
    case 'B': return "\xE7\x9B\xB8"; // rE.svg: 相
    case 'A': return "\xE9\xA9\xAC"; // white horse: 马
    case 'K': return "\xE8\xBD\xA6"; // white rook: 车
    case 'C': return "\xE7\x82\xAE"; // rC.svg: 炮
    case 'P': return "\xE5\x85\xB5"; // rS.svg: 兵
    default: return nullptr;
    }
}
void clearCircle(uint8_t *f, int cx, int cy, int radius) {
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x)
            if (x * x + y * y <= radius * radius) {
                const int px = cx + x;
                const int py = cy + y;
                if (px >= 0 && px < XingtaiEpd::WIDTH && py >= 0 && py < XingtaiEpd::HEIGHT)
                    f[static_cast<size_t>(py) * 30 + px / 8] &= static_cast<uint8_t>(~(0x80U >> (px % 8)));
            }
}
void fillCircle(uint8_t *f, int cx, int cy, int radius) {
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x)
            if (x * x + y * y <= radius * radius) pixel(f, cx + x, cy + y);
}
void circleOutline(uint8_t *f, int cx, int cy, int radius) {
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        pixel(f, cx + x, cy + y);
        pixel(f, cx + y, cy + x);
        pixel(f, cx - y, cy + x);
        pixel(f, cx - x, cy + y);
        pixel(f, cx - x, cy - y);
        pixel(f, cx - y, cy - x);
        pixel(f, cx + y, cy - x);
        pixel(f, cx + x, cy - y);
        ++y;
        if (error < 0) {
            error += 2 * y + 1;
        } else {
            --x;
            error += 2 * (y - x) + 1;
        }
    }
}
void drawNewGameIcon(uint8_t *f, int cx, int cy, int r) {
    // Refresh arrow: a circle open at the top-right with an arrowhead.
    int x = r;
    int y = 0;
    int error = 1 - r;
    while (x >= y) {
        pixel(f, cx - x, cy - y);
        pixel(f, cx - y, cy - x);
        pixel(f, cx - y, cy + x);
        pixel(f, cx - x, cy + y);
        pixel(f, cx + x, cy + y);
        pixel(f, cx + y, cy + x);
        ++y;
        if (error < 0) {
            error += 2 * y + 1;
        } else {
            --x;
            error += 2 * (y - x) + 1;
        }
    }
    line(f, cx - 2, cy - r, cx + 2, cy - r);
    line(f, cx + 2, cy - r, cx, cy - r - 2);
    line(f, cx + 2, cy - r, cx, cy - r + 2);
}
void drawReturnIcon(uint8_t *f, int cx, int cy, int r) {
    circleOutline(f, cx, cy, r);
    line(f, cx - r + 3, cy, cx + r - 3, cy);
    line(f, cx - r + 3, cy, cx - r + 7, cy - 4);
    line(f, cx - r + 3, cy, cx - r + 7, cy + 4);
}
bool isBlackXiangqiPiece(char piece) { return piece >= 'a' && piece <= 'z'; }
bool isWhiteXiangqiPiece(char piece) { return piece >= 'A' && piece <= 'Z'; }
void drawSymbol(uint8_t *f, int cx, int cy, const char *symbol, bool inverted) {
    const int width = UiLocalization::textWidth(symbol, 1);
    const int textX = cx - width / 2;
    const int textY = cy - 8;
    if (!inverted) {
        UiLocalization::drawText(f, textX, textY, symbol, 1);
        return;
    }
    static uint8_t mask[XingtaiEpd::FRAME_BYTES];
    std::memset(mask, 0, sizeof(mask));
    UiLocalization::drawText(mask, textX, textY, symbol, 1);
    for (int y = cy - XIANGQI_PIECE_RADIUS; y <= cy + XIANGQI_PIECE_RADIUS; ++y) {
        if (y < 0 || y >= XingtaiEpd::HEIGHT) continue;
        for (int x = cx - XIANGQI_PIECE_RADIUS; x <= cx + XIANGQI_PIECE_RADIUS; ++x) {
            if (x < 0 || x >= XingtaiEpd::WIDTH) continue;
            const uint8_t bit = 0x80U >> (x % 8);
            if (mask[static_cast<size_t>(y) * 30 + x / 8] & bit)
                f[static_cast<size_t>(y) * 30 + x / 8] &= static_cast<uint8_t>(~bit);
        }
    }
}
void drawXiangqiPiece(uint8_t *f, int cx, int cy, char piece) {
    const char *symbol = xiangqiSymbol(piece);
    if (!symbol) return;
    clearCircle(f, cx, cy, XIANGQI_PIECE_RADIUS);
    if (isBlackXiangqiPiece(piece)) {
        fillCircle(f, cx, cy, XIANGQI_PIECE_RADIUS);
        drawSymbol(f, cx, cy, symbol, true);
    } else if (isWhiteXiangqiPiece(piece)) {
        circleOutline(f, cx, cy, XIANGQI_PIECE_RADIUS);
        drawSymbol(f, cx, cy, symbol, false);
    }
}
void drawPiece(uint8_t *f,int x,int y,char piece){if(piece==' ')return;char s[2]={piece,0};UiLocalization::drawText(f,x+9,y+9,s,2);}
void drawChess(uint8_t *f){for(int y=0;y<8;++y)for(int x=0;x<8;++x){int px=BOARD_X+x*CHESS_CELL,py=BOARD_Y+y*CHESS_CELL;rect(f,px,py,CHESS_CELL,CHESS_CELL);if(x==selectedX&&y==selectedY)rect(f,px+2,py+2,CHESS_CELL-4,CHESS_CELL-4);drawPiece(f,px,py,chess[y][x]);}}
void drawXiangqi(uint8_t *f) {
    // Geometry follows chinese_chess/board.svg: a 9x10 grid split by a river,
    // with palace diagonals and no vertical lines across the river.
    const int left = XIANGQI_LEFT;
    const int right = left + 8 * XIANGQI_CELL_X;
    const int top = XIANGQI_TOP;
    const int bottom = top + 9 * XIANGQI_CELL_Y;
    rect(f, left, top, right - left + 1, bottom - top + 1);
    for (int row = 1; row < 9; ++row) line(f, left, top + row * XIANGQI_CELL_Y, right, top + row * XIANGQI_CELL_Y);
    for (int column = 1; column < 8; ++column) {
        line(f, left + column * XIANGQI_CELL_X, top, left + column * XIANGQI_CELL_X, top + 4 * XIANGQI_CELL_Y);
        line(f, left + column * XIANGQI_CELL_X, top + 5 * XIANGQI_CELL_Y, left + column * XIANGQI_CELL_X, bottom);
    }
    line(f, left + 3 * XIANGQI_CELL_X, top, left + 5 * XIANGQI_CELL_X, top + 2 * XIANGQI_CELL_Y);
    line(f, left + 5 * XIANGQI_CELL_X, top, left + 3 * XIANGQI_CELL_X, top + 2 * XIANGQI_CELL_Y);
    line(f, left + 3 * XIANGQI_CELL_X, top + 7 * XIANGQI_CELL_Y, left + 5 * XIANGQI_CELL_X, bottom);
    line(f, left + 5 * XIANGQI_CELL_X, top + 7 * XIANGQI_CELL_Y, left + 3 * XIANGQI_CELL_X, bottom);
    UiLocalization::drawCentered(f, top + 9 * XIANGQI_CELL_Y / 2 - 8, "\xE6\xA5\x9A\xE6\xB2\xB3\xE6\xB1\x89\xE7\x95\x8C", 1);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 9; ++x) {
            const int cx = left + x * XIANGQI_CELL_X;
            const int cy = top + y * XIANGQI_CELL_Y;
            if (x == selectedX && y == selectedY) rect(f, cx - 10, cy - 10, 20, 20);
            drawXiangqiPiece(f, cx, cy, xiangqi[y][x]);
        }
}
}

namespace GamePage {
void setContentUrl(const char *url){copyText(contentUrl,sizeof(contentUrl),url);}
void open(){exitRequested=false;selectedGame=Game::List;copyText(feedback,sizeof(feedback),text("Choose a game","选择游戏"));}
bool returnControlAt(int16_t x,int16_t y){return inRect(x,y,10,40,48,28);}
bool takeExitRequest(){bool r=exitRequested;exitRequested=false;return r;}
bool handleTap(int16_t x,int16_t y){
    if(returnControlAt(x,y)){if(selectedGame==Game::List)exitRequested=true;else{selectedGame=Game::List;copyText(feedback,sizeof(feedback),text("Choose a game","选择游戏"));}return true;}
    if(selectedGame==Game::List){if(inRect(x,y,16,90,208,54)){selectedGame=Game::InternationalChess;resetBoard();copyText(feedback,sizeof(feedback),text("Your turn","轮到你走"));}else if(inRect(x,y,16,156,208,54)){selectedGame=Game::ChineseChess;resetBoard();copyText(feedback,sizeof(feedback),text("Chinese chess ready","象棋已准备"));}else if(inRect(x,y,16,222,208,54)){selectedGame=Game::DouDizhu;copyText(feedback,sizeof(feedback),text("Dou Dizhu ready","斗地主已准备"));}else return false;return true;}
    if(selectedGame==Game::DouDizhu){if(inRect(x,y,16,300,208,48))postMove("",false);return true;}
    if(selectedGame==Game::ChineseChess&&inRect(x,y,NEW_GAME_ICON_CX-16,NEW_GAME_ICON_CY-16,32,32)){resetBoard();copyText(feedback,sizeof(feedback),text("New game","新游戏"));return true;}
    const int cellX=selectedGame==Game::InternationalChess?CHESS_CELL:XIANGQI_CELL_X;
    const int cellY=selectedGame==Game::InternationalChess?CHESS_CELL:XIANGQI_CELL_Y;
    const int cols=selectedGame==Game::InternationalChess?8:9,rows=selectedGame==Game::InternationalChess?8:10;
    const int hitLeft=selectedGame==Game::InternationalChess?BOARD_X:XIANGQI_LEFT-XIANGQI_CELL_X/2;
    const int hitTop=selectedGame==Game::InternationalChess?BOARD_Y:XIANGQI_TOP-XIANGQI_CELL_Y/2;
    if(!inRect(x,y,hitLeft,hitTop,cols*cellX,rows*cellY)){
        if(selectedGame==Game::InternationalChess&&inRect(x,y,16,350,208,42))postMove("",false);
        return true;
    }
    int tx=(x-hitLeft)/cellX,ty=(y-hitTop)/cellY;
    if(selectedX<0){
        if(selectedGame==Game::InternationalChess&&(chess[ty][tx]<'A'||chess[ty][tx]>'Z')){copyText(feedback,sizeof(feedback),text("Select your white piece","请选择你的白棋"));return true;}
        if(selectedGame==Game::ChineseChess&&((redToMove&& !isWhiteXiangqiPiece(xiangqi[ty][tx]))||(!redToMove&&!isBlackXiangqiPiece(xiangqi[ty][tx])))){copyText(feedback,sizeof(feedback),text("Select the side to move","请选择当前方棋子"));return true;}
        selectedX=tx;selectedY=ty;return true;
    }
    String move;if(selectedGame==Game::InternationalChess){char previousTarget=chess[ty][tx];move+=(char)('a'+selectedX);move+=String(8-selectedY);move+=(char)('a'+tx);move+=String(8-ty);char p=chess[selectedY][selectedX];chess[ty][tx]=p;chess[selectedY][selectedX]=' ';whiteToMove=false;selectedX=selectedY=-1;if(!postMove(move,true)){chess[(8-(move[1]-'0'))][move[0]-'a']=p;chess[ty][tx]=previousTarget;whiteToMove=true;}return true;}
    else{move=String(selectedX)+","+String(selectedY)+","+String(tx)+","+String(ty);char p=xiangqi[selectedY][selectedX];xiangqi[ty][tx]=p;xiangqi[selectedY][selectedX]=' ';redToMove=!redToMove;copyText(feedback,sizeof(feedback),text("Move recorded","已记录走法"));}
    selectedX=selectedY=-1;postMove(move,true);return true;
}
void render(uint8_t *f){
    std::memset(f,0,XingtaiEpd::FRAME_BYTES);
    if(selectedGame==Game::List){
        const char* names[]={text("INTERNATIONAL CHESS","国际象棋"),text("CHINESE CHESS","中国象棋"),text("DOU DIZHU","斗地主")};
        for(int i=0;i<3;++i){rect(f,16,90+i*66,208,54);UiLocalization::drawCentered(f,112+i*66,names[i],1);}
        UiLocalization::drawCentered(f,330,feedback,1);return;
    }
    if(selectedGame!=Game::ChineseChess){
        const char* title=selectedGame==Game::InternationalChess?text("CHESS","国际象棋"):text("DOU DIZHU","斗地主");
        UiLocalization::drawText(f,10,42,title,1);
    }
    if(selectedGame==Game::InternationalChess){
        drawChess(f);
        UiLocalization::drawText(f,8,306,text("YOU: WHITE","你：白方"),1);
        UiLocalization::drawText(f,132,306,text("AI: BLACK","AI：黑方"),1);
    } else if(selectedGame==Game::ChineseChess){
        drawXiangqi(f);
        drawReturnIcon(f,RETURN_ICON_CX,RETURN_ICON_CY,GAME_ICON_RADIUS);
        drawNewGameIcon(f,NEW_GAME_ICON_CX,NEW_GAME_ICON_CY,GAME_ICON_RADIUS);
    } else {
        rect(f,16,100,208,150);UiLocalization::drawCentered(f,130,"3 3 4 5 6 7 8 9 J Q K A 2",1);UiLocalization::drawCentered(f,180,text("ASK AI FOR MOVE","请求 AI 走牌"),1);
        rect(f,16,350,208,42);UiLocalization::drawCentered(f,365,text("ASK AI","请求 AI"),1);UiLocalization::drawCentered(f,400,feedback,1);
    }
}
void drawTopbar(uint8_t *frame){
    clearRect(frame,28,0,152,32);
}
}