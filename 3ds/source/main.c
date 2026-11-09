/*
 * FireTV Remote for 3DS
 * Talks ADB directly to a Fire TV over Wi-Fi. No PC or Pi needed.
 * UI drawn with citro2d.
 */
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include "adb.h"

#define SOC_ALIGN        0x1000
#define SOC_BUFFERSIZE   0x100000
#define APP_DIR          "sdmc:/3ds/firetv-remote"
#define CFG_PATH         APP_DIR "/tv-ip.txt"
#define KEY_PATH         APP_DIR "/adbkey"
#define ADB_PORT         5555
#define AUTH_WAIT_MS     90000
#define LONGPRESS_FRAMES 35        /* ~0.6 s */

/* ---------- palette ---------- */
/* Same as C2D_Color32, but usable in static initializers. */
#define RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))
#define COL_BG_TOP    RGBA(0x12, 0x14, 0x1C, 0xFF)
#define COL_BG_BOT    RGBA(0x1C, 0x1F, 0x2B, 0xFF)
#define COL_PANEL     RGBA(0x23, 0x27, 0x33, 0xFF)
#define COL_PANEL_HI  RGBA(0x2E, 0x33, 0x42, 0xFF)
#define COL_ACCENT    RGBA(0xFF, 0x99, 0x00, 0xFF)
#define COL_TEXT      RGBA(0xE8, 0xEA, 0xF0, 0xFF)
#define COL_DIM       RGBA(0x8A, 0x90, 0xA2, 0xFF)
#define COL_GREEN     RGBA(0x3D, 0xDC, 0x84, 0xFF)
#define COL_RED       RGBA(0xFF, 0x5A, 0x5F, 0xFF)
#define COL_SHADOW    RGBA(0x00, 0x00, 0x00, 0x60)

/* Android keycodes */
enum {
    K_HOME = 3, K_BACK = 4, K_UP = 19, K_DOWN = 20, K_LEFT = 21, K_RIGHT = 22,
    K_SELECT = 23, K_VOLUP = 24, K_VOLDOWN = 25, K_MENU = 82, K_SEARCH = 84,
    K_PLAYPAUSE = 85, K_NEXT = 87, K_PREV = 88, K_REWIND = 89, K_FF = 90,
    K_MUTE = 164, K_SLEEP = 223, K_WAKEUP = 224,
};

/* ---------- buttons on the touch screen ---------- */
typedef enum { ACT_KEY, ACT_SHELL, ACT_TYPE, ACT_SCAN, ACT_SETIP, ACT_EXIT } ActType;
typedef enum { IC_NONE, IC_HOME, IC_BACK, IC_MENU, IC_SEARCH, IC_REW, IC_PLAY, IC_FF,
               IC_MUTE, IC_VOLDN, IC_VOLUP, IC_SLEEP, IC_WAKE, IC_KEYBOARD } Icon;

typedef struct {
    float x, y, w, h;
    const char *label;
    ActType type;
    int key;
    const char *shell;
    Icon icon;
    u32 color;          /* 0 = default panel */
    bool repeat;        /* auto-repeat while held */
    bool needs_tv;
    C2D_Text text;
} Button;

#define GX 7.0f
#define GW 70.0f
#define GH 40.0f
#define COLX(c) (GX + (c) * (GW + GX))
#define ROWY(r) (8.0f + (r) * (GH + GX))

static Button buttons[] = {
    { COLX(0), ROWY(0), GW, GH, "Home",   ACT_KEY, K_HOME,      0, IC_HOME,   0, false, true },
    { COLX(1), ROWY(0), GW, GH, "Back",   ACT_KEY, K_BACK,      0, IC_BACK,   0, false, true },
    { COLX(2), ROWY(0), GW, GH, "Menu",   ACT_KEY, K_MENU,      0, IC_MENU,   0, false, true },
    { COLX(3), ROWY(0), GW, GH, "Search", ACT_KEY, K_SEARCH,    0, IC_SEARCH, 0, false, true },

    { COLX(0), ROWY(1), GW, GH, "Rewind", ACT_KEY, K_REWIND,    0, IC_REW,    0, true,  true },
    { COLX(1), ROWY(1), GW, GH, "Play",   ACT_KEY, K_PLAYPAUSE, 0, IC_PLAY,   0, false, true },
    { COLX(2), ROWY(1), GW, GH, "Forward",ACT_KEY, K_FF,        0, IC_FF,     0, true,  true },
    { COLX(3), ROWY(1), GW, GH, "Mute",   ACT_KEY, K_MUTE,      0, IC_MUTE,   0, false, true },

    { COLX(0), ROWY(2), GW, GH, "Vol -",  ACT_KEY, K_VOLDOWN,   0, IC_VOLDN,  0, true,  true },
    { COLX(1), ROWY(2), GW, GH, "Vol +",  ACT_KEY, K_VOLUP,     0, IC_VOLUP,  0, true,  true },
    { COLX(2), ROWY(2), GW, GH, "Sleep",  ACT_KEY, K_SLEEP,     0, IC_SLEEP,  0, false, true },
    { COLX(3), ROWY(2), GW, GH, "Wake",   ACT_KEY, K_WAKEUP,    0, IC_WAKE,   0, false, true },

    { COLX(0), ROWY(3), GW, GH, "Netflix", ACT_SHELL, 0, "monkey -p com.netflix.ninja -c android.intent.category.LAUNCHER 1",
      IC_NONE, RGBA(0x8E, 0x1B, 0x22, 0xFF), false, true },
    { COLX(1), ROWY(3), GW, GH, "YouTube", ACT_SHELL, 0, "monkey -p com.amazon.firetv.youtube -c android.intent.category.LAUNCHER 1",
      IC_NONE, RGBA(0x9A, 0x2A, 0x2A, 0xFF), false, true },
    { COLX(2), ROWY(3), GW, GH, "Settings", ACT_SHELL, 0, "am start -a android.settings.SETTINGS",
      IC_NONE, RGBA(0x2F, 0x4B, 0x7A, 0xFF), false, true },
    { COLX(3), ROWY(3), GW, GH, "Type",   ACT_TYPE, 0, 0, IC_KEYBOARD, 0, false, true },

    { 7,   202, 98, 30, "Scan for TV", ACT_SCAN,  0, 0, IC_NONE, COL_PANEL_HI, false, false },
    { 111, 202, 98, 30, "Set TV IP",   ACT_SETIP, 0, 0, IC_NONE, COL_PANEL_HI, false, false },
    { 215, 202, 98, 30, "Exit",        ACT_EXIT,  0, 0, IC_NONE, RGBA(0x4A, 0x24, 0x2A, 0xFF), false, false },
};
#define NBUTTONS (int)(sizeof(buttons) / sizeof(buttons[0]))

/* ---------- state ---------- */
static C3D_RenderTarget *scr_top, *scr_bot;
static C2D_TextBuf static_buf, dyn_buf;
static u32 *soc_buf = NULL;
static char tv_ip[16] = "";
static char last_action[48] = "";
static int  action_flash = 0;        /* frames left for the "last action" pulse */
static int  pressed_btn = -1;        /* touch button being held */
static int  btn_flash[NBUTTONS];     /* highlight timer per button */
static u32  frame_count = 0;

/* ---------- drawing primitives ---------- */
static void rounded_rect(float x, float y, float w, float h, float r, u32 c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    C2D_DrawRectSolid(x + r, y, 0, w - 2 * r, h, c);
    C2D_DrawRectSolid(x, y + r, 0, r, h - 2 * r, c);
    C2D_DrawRectSolid(x + w - r, y + r, 0, r, h - 2 * r, c);
    C2D_DrawCircleSolid(x + r,     y + r,     0, r, c);
    C2D_DrawCircleSolid(x + w - r, y + r,     0, r, c);
    C2D_DrawCircleSolid(x + r,     y + h - r, 0, r, c);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0, r, c);
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c) {
    C2D_DrawTriangle(x0, y0, c, x1, y1, c, x2, y2, c, 0);
}

static void line(float x0, float y0, float x1, float y1, float t, u32 c) {
    C2D_DrawLine(x0, y0, c, x1, y1, c, t, 0);
}

static void text(const char *s, float x, float y, float scale, u32 color, bool center) {
    C2D_Text t;
    C2D_TextParse(&t, dyn_buf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | (center ? C2D_AlignCenter : 0), x, y, 0, scale, scale, color);
}

/* Small vector icons, centred on (cx, cy), about 16 px tall. */
static void draw_icon(Icon ic, float cx, float cy, u32 c, u32 bg) {
    switch (ic) {
    case IC_HOME:
        tri(cx - 9, cy, cx, cy - 8, cx + 9, cy, c);
        C2D_DrawRectSolid(cx - 6, cy, 0, 12, 8, c);
        C2D_DrawRectSolid(cx - 2, cy + 3, 0, 4, 5, bg);
        break;
    case IC_BACK:
        tri(cx - 9, cy, cx - 2, cy - 7, cx - 2, cy + 7, c);
        C2D_DrawRectSolid(cx - 3, cy - 2.5f, 0, 11, 5, c);
        break;
    case IC_MENU:
        for (int i = -1; i <= 1; i++) C2D_DrawRectSolid(cx - 8, cy + i * 5 - 1.5f, 0, 16, 3, c);
        break;
    case IC_SEARCH:
        C2D_DrawCircleSolid(cx - 2, cy - 2, 0, 6.5f, c);
        C2D_DrawCircleSolid(cx - 2, cy - 2, 0, 4, bg);
        line(cx + 2, cy + 2, cx + 8, cy + 8, 3, c);
        break;
    case IC_REW:
        tri(cx - 9, cy, cx, cy - 6, cx, cy + 6, c);
        tri(cx, cy, cx + 9, cy - 6, cx + 9, cy + 6, c);
        break;
    case IC_FF:
        tri(cx + 9, cy, cx, cy - 6, cx, cy + 6, c);
        tri(cx, cy, cx - 9, cy - 6, cx - 9, cy + 6, c);
        break;
    case IC_PLAY:
        tri(cx - 9, cy - 7, cx - 9, cy + 7, cx + 1, cy, c);
        C2D_DrawRectSolid(cx + 3, cy - 7, 0, 3, 14, c);
        C2D_DrawRectSolid(cx + 8, cy - 7, 0, 3, 14, c);
        break;
    case IC_MUTE:
    case IC_VOLDN:
    case IC_VOLUP: {
        float sx = cx - 6;
        C2D_DrawRectSolid(sx - 4, cy - 3, 0, 4, 6, c);
        tri(sx - 1, cy, sx + 5, cy - 7, sx + 5, cy + 7, c);
        if (ic == IC_MUTE) {
            line(cx + 2, cy - 4, cx + 9, cy + 4, 2, c);
            line(cx + 2, cy + 4, cx + 9, cy - 4, 2, c);
        } else {
            C2D_DrawRectSolid(cx + 2, cy - 1.25f, 0, 8, 2.5f, c);
            if (ic == IC_VOLUP) C2D_DrawRectSolid(cx + 4.75f, cy - 4, 0, 2.5f, 8, c);
        }
        break;
    }
    case IC_SLEEP:
        C2D_DrawCircleSolid(cx, cy, 0, 7, c);
        C2D_DrawCircleSolid(cx + 4, cy - 3, 0, 6, bg);
        break;
    case IC_WAKE:
        C2D_DrawCircleSolid(cx, cy, 0, 4, c);
        for (int i = 0; i < 8; i++) {
            float a = i * (float)M_PI / 4;
            line(cx + cosf(a) * 6, cy + sinf(a) * 6, cx + cosf(a) * 9, cy + sinf(a) * 9, 2, c);
        }
        break;
    case IC_KEYBOARD:
        rounded_rect(cx - 11, cy - 7, 22, 14, 2, c);
        for (int r = 0; r < 2; r++)
            for (int k = 0; k < 4; k++)
                C2D_DrawRectSolid(cx - 9 + k * 5, cy - 5 + r * 4, 0, 3, 2.5f, bg);
        C2D_DrawRectSolid(cx - 6, cy + 3, 0, 12, 2, bg);
        break;
    default:
        break;
    }
}

static void draw_button(Button *b, int idx, bool tv_ok) {
    bool active   = !b->needs_tv || tv_ok;
    bool lit      = (pressed_btn == idx) || btn_flash[idx] > 0;
    u32  base     = b->color ? b->color : COL_PANEL;
    u32  fill     = lit ? COL_ACCENT : base;
    u32  fg       = lit ? RGBA(0x1A, 0x10, 0x00, 0xFF) : (active ? COL_TEXT : COL_DIM);
    float press   = lit ? 1.5f : 0;

    rounded_rect(b->x, b->y + 2, b->w, b->h, 8, COL_SHADOW);
    rounded_rect(b->x, b->y + press, b->w, b->h, 8, fill);

    if (b->icon != IC_NONE) {
        draw_icon(b->icon, b->x + b->w / 2, b->y + 15 + press, fg, fill);
        C2D_DrawText(&b->text, C2D_WithColor | C2D_AlignCenter,
                     b->x + b->w / 2, b->y + b->h - 15 + press, 0, 0.42f, 0.42f, fg);
    } else {
        float tw, th;
        C2D_TextGetDimensions(&b->text, 0.5f, 0.5f, &tw, &th);
        C2D_DrawText(&b->text, C2D_WithColor | C2D_AlignCenter,
                     b->x + b->w / 2, b->y + (b->h - th) / 2 + press, 0, 0.5f, 0.5f, fg);
    }
}

static void draw_bottom(void) {
    bool tv_ok = adb_is_connected();
    C2D_TargetClear(scr_bot, COL_BG_BOT);
    C2D_SceneBegin(scr_bot);
    for (int i = 0; i < NBUTTONS; i++) draw_button(&buttons[i], i, tv_ok);
}

static void draw_spinner(float cx, float cy) {
    for (int i = 0; i < 8; i++) {
        float a = (i / 8.0f) * 2 * (float)M_PI + frame_count * 0.12f;
        u8 alpha = 40 + i * 26;
        C2D_DrawCircleSolid(cx + cosf(a) * 16, cy + sinf(a) * 16, 0, 3.5f,
                            RGBA(0xFF, 0x99, 0x00, alpha));
    }
}

/* Top screen. `status` != NULL shows a busy overlay with a spinner. */
static void draw_top(const char *status) {
    char buf[96];
    bool tv_ok = adb_is_connected();

    C2D_TargetClear(scr_top, COL_BG_TOP);
    C2D_SceneBegin(scr_top);

    /* header */
    C2D_DrawRectSolid(0, 0, 0, 400, 30, COL_PANEL);
    C2D_DrawRectSolid(0, 30, 0, 400, 2, COL_ACCENT);
    rounded_rect(10, 7, 16, 16, 3, COL_ACCENT);
    C2D_DrawRectSolid(14, 11, 0, 8, 6, COL_PANEL);
    text("FireTV Remote", 34, 6, 0.6f, COL_TEXT, false);

    /* connection pill */
    const char *pill = status ? "Working..." : tv_ok ? adb_device_name() : "Offline";
    u32 pc = status ? COL_ACCENT : tv_ok ? COL_GREEN : COL_RED;
    C2D_Text pt;
    C2D_TextParse(&pt, dyn_buf, pill);
    float pw, ph;
    C2D_TextGetDimensions(&pt, 0.45f, 0.45f, &pw, &ph);
    float px = 392 - pw - 22;
    rounded_rect(px, 7, pw + 22, 17, 8.5f, COL_PANEL_HI);
    C2D_DrawCircleSolid(px + 9, 15.5f, 0, 3.5f, pc);
    C2D_DrawText(&pt, C2D_WithColor, px + 16, 8, 0, 0.45f, 0.45f, COL_TEXT);

    /* centre card */
    rounded_rect(20, 44, 360, 100, 12, COL_PANEL);
    if (status) {
        draw_spinner(60, 94);
        C2D_Text st;
        C2D_TextParse(&st, dyn_buf, status);
        C2D_DrawText(&st, C2D_WithColor | C2D_WordWrap, 90, 70, 0, 0.55f, 0.55f, COL_TEXT, 275.0f);
        text(" Cancel", 370, 124, 0.42f, COL_DIM, false);
    } else if (tv_ok) {
        float pulse = action_flash > 0 ? action_flash / 20.0f : 0;
        if (pulse > 0) {
            u8 a = (u8)(pulse * 90);
            rounded_rect(20 - pulse * 4, 44 - pulse * 4, 360 + pulse * 8, 100 + pulse * 8, 14,
                         RGBA(0xFF, 0x99, 0x00, a));
            rounded_rect(20, 44, 360, 100, 12, COL_PANEL);
        }
        text(last_action[0] ? last_action : "Ready", 200, 66, 0.95f,
             pulse > 0 ? COL_ACCENT : COL_TEXT, true);
        snprintf(buf, sizeof(buf), "Connected to %s", tv_ip);
        text(buf, 200, 112, 0.45f, COL_DIM, true);
    } else {
        text("Not connected", 200, 56, 0.8f, COL_RED, true);
        const char *err = adb_last_error();
        C2D_Text et;
        C2D_TextParse(&et, dyn_buf, err[0] ? err : (tv_ip[0] ? "Tap Scan or Set TV IP if this is wrong" : "No TV selected yet"));
        C2D_DrawText(&et, C2D_WithColor | C2D_AlignCenter | C2D_WordWrap, 200, 86, 0, 0.45f, 0.45f, COL_DIM, 330.0f);
        text("Press  to connect", 200, 118, 0.55f, COL_TEXT, true);
    }

    /* controls legend */
    static const char *legend[][2] = {
        { "",          "Navigate" },     { "",          "Select (hold: long)" },
        { "",          "Back" },         { " / ", "Home / Menu" },
        { "START",           "Play / Pause" }, { " / ", "Rewind / Forward" },
        { "ZL / ZR",         "Volume" },       { "SELECT",          "Type text" },
    };
    for (int i = 0; i < 8; i++) {
        float x = (i % 2) ? 210 : 24;
        float y = 154 + (i / 2) * 20;
        text(legend[i][0], x, y, 0.5f, COL_ACCENT, false);
        text(legend[i][1], x + 58, y + 1, 0.48f, COL_TEXT, false);
    }
}

static void render(const char *status) {
    C2D_TextBufClear(dyn_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    draw_top(status);
    draw_bottom();
    C3D_FrameEnd(0);
    frame_count++;
}

/* Progress callback for connect / scan / keygen: keep animating, allow B to cancel. */
static bool on_progress(const char *msg, void *user) {
    (void)user;
    if (!aptMainLoop()) return false;
    hidScanInput();
    if (hidKeysDown() & KEY_B) return false;
    render(msg);
    return true;
}

/* ---------- config ---------- */
static void save_ip(void) {
    FILE *f = fopen(CFG_PATH, "w");
    if (f) { fprintf(f, "%s\n", tv_ip); fclose(f); }
}

static bool load_ip(void) {
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return false;
    bool ok = fgets(tv_ip, sizeof(tv_ip), f) != NULL;
    fclose(f);
    tv_ip[strcspn(tv_ip, "\r\n ")] = 0;
    struct in_addr a;
    if (!ok || !inet_aton(tv_ip, &a)) { tv_ip[0] = 0; return false; }
    return true;
}

/* ---------- actions ---------- */
static void note(const char *what) {
    snprintf(last_action, sizeof(last_action), "%s", what);
    action_flash = 20;
}

static void send_key(int code, const char *name) {
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "input keyevent %d", code);
    adb_queue(cmd);
    note(name);
}

static void send_longpress(int code, const char *name) {
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "input keyevent --longpress %d", code);
    adb_queue(cmd);
    note(name);
}

static bool connect_tv(void) {
    if (!tv_ip[0]) return false;
    char msg[64];
    snprintf(msg, sizeof(msg), "Connecting to %s...", tv_ip);
    on_progress(msg, NULL);
    bool ok = adb_connect(tv_ip, ADB_PORT, AUTH_WAIT_MS, on_progress, NULL) == 0;
    if (ok) note("Connected!");
    return ok;
}

static bool prompt_ip(void) {
    SwkbdState kb;
    char ip[16] = {0};
    swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, 15);
    swkbdSetNumpadKeys(&kb, L'.', 0);
    swkbdSetHintText(&kb, "Fire TV IP, e.g. 192.168.1.50");
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (tv_ip[0]) swkbdSetInitialText(&kb, tv_ip);
    if (swkbdInputText(&kb, ip, sizeof(ip)) != SWKBD_BUTTON_CONFIRM) return false;
    struct in_addr a;
    if (!inet_aton(ip, &a)) return false;
    snprintf(tv_ip, sizeof(tv_ip), "%s", ip);
    save_ip();
    return true;
}

/* Scan the 3DS's /24 subnet for ADB (port 5555). Falls back to asking. */
static bool scan_tv(void) {
    struct in_addr me;
    me.s_addr = (in_addr_t)gethostid();
    if (me.s_addr != 0) {
        char found[8][16];
        int n = adb_scan(inet_ntoa(me), ADB_PORT, found, 8, on_progress, NULL);
        if (n < 0) return false;                       /* cancelled */
        if (n > 0) {
            snprintf(tv_ip, sizeof(tv_ip), "%s", found[0]);
            save_ip();
            return true;
        }
    }
    return prompt_ip();
}

/* `input text` treats %s as a space; quote everything else for sh. */
static void type_text(void) {
    SwkbdState kb;
    char str[121] = {0};
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 120);
    swkbdSetHintText(&kb, "Text to type on the TV");
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, str, sizeof(str)) != SWKBD_BUTTON_CONFIRM) return;

    char cmd[256] = "input text '";
    size_t o = strlen(cmd);
    for (const char *p = str; *p && o < sizeof(cmd) - 8; p++) {
        if (*p == ' ')       { memcpy(cmd + o, "%s", 2); o += 2; }
        else if (*p == '\'') { memcpy(cmd + o, "'\\''", 4); o += 4; }
        else if (*p == '%')  { memcpy(cmd + o, "\\%", 2); o += 2; }
        else                 cmd[o++] = *p;
    }
    cmd[o++] = '\'';
    cmd[o] = 0;
    adb_queue(cmd);
    note("Typed text");
}

/* Returns false if the app should exit. */
static bool run_button(Button *b) {
    bool tv_ok = adb_is_connected();
    switch (b->type) {
    case ACT_KEY:   if (tv_ok) send_key(b->key, b->label); break;
    case ACT_SHELL:
        if (tv_ok) {
            char what[48];
            adb_queue(b->shell);
            snprintf(what, sizeof(what), "Open %s", b->label);
            note(what);
        }
        break;
    case ACT_TYPE:  if (tv_ok) type_text(); break;
    case ACT_SCAN:  adb_disconnect(); if (scan_tv()) connect_tv(); break;
    case ACT_SETIP: adb_disconnect(); if (prompt_ip()) connect_tv(); break;
    case ACT_EXIT:  return false;
    }
    return true;
}

static int hit_test(const touchPosition *t) {
    for (int i = 0; i < NBUTTONS; i++) {
        Button *b = &buttons[i];
        if (t->px >= b->x && t->px < b->x + b->w && t->py >= b->y && t->py < b->y + b->h) return i;
    }
    return -1;
}

/* ---------- main ---------- */
int main(void) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    scr_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    scr_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    static_buf = C2D_TextBufNew(1024);
    dyn_buf    = C2D_TextBufNew(4096);
    for (int i = 0; i < NBUTTONS; i++) {
        C2D_TextParse(&buttons[i].text, static_buf, buttons[i].label);
        C2D_TextOptimize(&buttons[i].text);
    }

    osSetSpeedupEnable(true);           /* faster key generation on New 3DS */
    mkdir("sdmc:/3ds", 0777);
    mkdir(APP_DIR, 0777);

    bool net_ok = false;
    render("Starting Wi-Fi...");
    soc_buf = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (soc_buf && R_SUCCEEDED(socInit(soc_buf, SOC_BUFFERSIZE))) {
        net_ok = true;
        render("Preparing ADB key...\nFirst launch on an original 3DS can take a few minutes.");
        if (adb_init(KEY_PATH, on_progress, NULL) == 0) {
            if (load_ip() || scan_tv()) connect_tv();
        }
    }

    hidSetRepeatParameters(15, 5);      /* frames: initial delay, repeat rate */
    int a_frames = 0;
    bool a_long_sent = false;
    int touch_hold = 0;
    bool running = true;

    while (running && aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown(), rep = hidKeysDownRepeat();
        u32 held = hidKeysHeld(), up = hidKeysUp();
        bool tv_ok = adb_is_connected();

        if (!net_ok) {
            if (down & (KEY_START | KEY_B)) break;
            render("Wi-Fi isn't available.\nTurn on Wi-Fi and restart the app. START: exit");
            continue;
        }

        /* A fresh press always counts. Auto-repeat from a held button only fires
         * when the TV has caught up, so holding never builds a backlog and the
         * movement stops as soon as the button is released. */
        bool idle = adb_idle();
        #define PRESSED(k) ((down & (k)) || ((rep & (k)) && idle))

        if (tv_ok) {
            if (PRESSED(KEY_UP))    send_key(K_UP, "Up");
            if (PRESSED(KEY_DOWN))  send_key(K_DOWN, "Down");
            if (PRESSED(KEY_LEFT))  send_key(K_LEFT, "Left");
            if (PRESSED(KEY_RIGHT)) send_key(K_RIGHT, "Right");

            if (held & KEY_A) {
                if (++a_frames == LONGPRESS_FRAMES && !a_long_sent) {
                    send_longpress(K_SELECT, "Long press");
                    a_long_sent = true;
                }
            }
            if (up & KEY_A) {
                if (!a_long_sent && a_frames > 0) send_key(K_SELECT, "Select");
                a_frames = 0;
                a_long_sent = false;
            }
            if (down & KEY_B)     send_key(K_BACK, "Back");
            if (down & KEY_X)     send_key(K_HOME, "Home");
            if (down & KEY_Y)     send_key(K_MENU, "Menu");
            if (down & KEY_START) send_key(K_PLAYPAUSE, "Play / Pause");
            if (PRESSED(KEY_L))   send_key(K_REWIND, "Rewind");
            if (PRESSED(KEY_R))   send_key(K_FF, "Forward");
            if (PRESSED(KEY_ZL))  send_key(K_VOLDOWN, "Vol -");
            if (PRESSED(KEY_ZR))  send_key(K_VOLUP, "Vol +");
            if (down & KEY_CSTICK_LEFT)  send_key(K_PREV, "Previous");
            if (down & KEY_CSTICK_RIGHT) send_key(K_NEXT, "Next");
            if (down & KEY_SELECT) type_text();
        } else if (down & KEY_A) {
            if (tv_ip[0] || scan_tv()) connect_tv();
            a_frames = 0;               /* don't turn this press into a select */
        }

        /* touch screen */
        touchPosition t;
        hidTouchRead(&t);
        if (down & KEY_TOUCH) {
            pressed_btn = hit_test(&t);
            touch_hold = 0;
            if (pressed_btn >= 0) {
                btn_flash[pressed_btn] = 6;
                running = run_button(&buttons[pressed_btn]);
            }
        } else if ((held & KEY_TOUCH) && pressed_btn >= 0) {
            if (hit_test(&t) != pressed_btn) {
                pressed_btn = -1;       /* finger slid off */
            } else if (buttons[pressed_btn].repeat && ++touch_hold > 18 && touch_hold % 5 == 0 && idle) {
                run_button(&buttons[pressed_btn]);
            }
        } else if (up & KEY_TOUCH) {
            pressed_btn = -1;
        }

        for (int i = 0; i < NBUTTONS; i++) if (btn_flash[i] > 0) btn_flash[i]--;
        if (action_flash > 0) action_flash--;

        adb_poll();
        render(NULL);
    }

    adb_shutdown();
    if (net_ok) socExit();
    free(soc_buf);
    C2D_TextBufDelete(dyn_buf);
    C2D_TextBufDelete(static_buf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
