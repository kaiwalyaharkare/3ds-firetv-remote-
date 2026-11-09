/*
 * FireTV Remote for 3DS
 * Talks ADB directly to a Fire TV over Wi-Fi. No PC or Pi needed.
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include "adb.h"

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000
#define APP_DIR        "sdmc:/3ds/firetv-remote"
#define CFG_PATH       APP_DIR "/tv-ip.txt"
#define KEY_PATH       APP_DIR "/adbkey"
#define ADB_PORT       5555
#define AUTH_WAIT_MS   90000
#define LONGPRESS_FRAMES 35       /* ~0.6 s */

/* Android keycodes */
enum {
    K_HOME = 3, K_BACK = 4, K_UP = 19, K_DOWN = 20, K_LEFT = 21, K_RIGHT = 22,
    K_SELECT = 23, K_VOLUP = 24, K_VOLDOWN = 25, K_MENU = 82, K_SEARCH = 84,
    K_PLAYPAUSE = 85, K_NEXT = 87, K_PREV = 88, K_REWIND = 89, K_FF = 90,
    K_MUTE = 164, K_SLEEP = 223, K_WAKEUP = 224,
};

static PrintConsole top, bot;
static u32 *soc_buf = NULL;
static char tv_ip[16] = "";
static char last_action[48] = "";

/* ---------- touch buttons (bottom screen: 40x30 cells of 8x8 px) ---------- */
typedef enum { ACT_KEY, ACT_SHELL, ACT_TYPE, ACT_SETTV, ACT_EXIT } ActType;
typedef struct { int row, col; const char *label; ActType type; int key; const char *shell; } Button;

#define BW 12
#define BH 3
static const Button buttons[] = {
    { 3, 1, "HOME",       ACT_KEY, K_HOME, 0 },   { 3, 14, "BACK",  ACT_KEY, K_BACK, 0 },   { 3, 27, "MENU",  ACT_KEY, K_MENU, 0 },
    { 7, 1, "<< REW",     ACT_KEY, K_REWIND, 0 }, { 7, 14, "PLAY/PAUSE", ACT_KEY, K_PLAYPAUSE, 0 }, { 7, 27, "FF >>", ACT_KEY, K_FF, 0 },
    {11, 1, "VOL -",      ACT_KEY, K_VOLDOWN, 0 },{11, 14, "MUTE",  ACT_KEY, K_MUTE, 0 },   {11, 27, "VOL +", ACT_KEY, K_VOLUP, 0 },
    {15, 1, "NETFLIX",    ACT_SHELL, 0, "monkey -p com.netflix.ninja -c android.intent.category.LAUNCHER 1" },
    {15, 14, "YOUTUBE",   ACT_SHELL, 0, "monkey -p com.amazon.firetv.youtube -c android.intent.category.LAUNCHER 1" },
    {15, 27, "SETTINGS",  ACT_SHELL, 0, "am start -a android.settings.SETTINGS" },
    {19, 1, "TYPE TEXT",  ACT_TYPE, 0, 0 },       {19, 14, "SLEEP", ACT_KEY, K_SLEEP, 0 },  {19, 27, "WAKE",  ACT_KEY, K_WAKEUP, 0 },
    {23, 1, "SET TV IP",  ACT_SETTV, 0, 0 },      {23, 14, "SEARCH", ACT_KEY, K_SEARCH, 0 },{23, 27, "EXIT",  ACT_EXIT, 0, 0 },
};
#define NBUTTONS (int)(sizeof(buttons) / sizeof(buttons[0]))

/* ---------- drawing ---------- */
static void at(PrintConsole *c, int row, int col) {
    consoleSelect(c);
    c->cursorX = col;
    c->cursorY = row;
}

static void present(void) {
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static void draw_button(const Button *b, bool pressed) {
    char line[BW + 1];
    const char *color = pressed ? "\x1b[33m" : "\x1b[36m";
    int len = (int)strlen(b->label);
    if (len > BW - 2) len = BW - 2;
    int pad = (BW - 2 - len) / 2;

    memset(line, '-', BW); line[0] = '+'; line[BW - 1] = '+'; line[BW] = 0;
    at(&bot, b->row, b->col);     printf("%s%s", color, line);
    at(&bot, b->row + 2, b->col); printf("%s", line);

    memset(line, ' ', BW); line[0] = '|'; line[BW - 1] = '|'; line[BW] = 0;
    memcpy(line + 1 + pad, b->label, len);
    at(&bot, b->row + 1, b->col); printf("%s\x1b[0m", line);
}

static void draw_bottom(void) {
    consoleSelect(&bot);
    consoleClear();
    at(&bot, 1, 11); printf("\x1b[37mFireTV Remote 3DS\x1b[0m");
    for (int i = 0; i < NBUTTONS; i++) draw_button(&buttons[i], false);
}

static void draw_top(const char *status) {
    consoleSelect(&top);
    consoleClear();
    at(&top, 0, 0);
    printf("\x1b[37m========= FireTV Remote for 3DS =========\x1b[0m\n\n");
    printf(" TV IP : %s\n", tv_ip[0] ? tv_ip : "(not set)");
    if (adb_is_connected())
        printf(" Status: \x1b[32mconnected\x1b[0m (%s)\n", adb_device_name());
    else
        printf(" Status: \x1b[31mdisconnected\x1b[0m\n");
    if (status && status[0])      printf("\n \x1b[33m%s\x1b[0m\n", status);
    else if (!adb_is_connected()) printf("\n \x1b[31m%.38s\x1b[0m\n Press A to connect.\n", adb_last_error());
    else                          printf("\n Last: %s\n", last_action);

    printf("\n\x1b[37m Controls\x1b[0m\n");
    printf("  D-Pad / Circle Pad   navigate\n");
    printf("  A  select   (hold = long press)\n");
    printf("  B  back      X  home      Y  menu\n");
    printf("  START   play / pause\n");
    printf("  L / R   rewind / fast-forward\n");
    printf("  ZL / ZR volume  (New 3DS)\n");
    printf("  C-Stick L/R  prev / next\n");
    printf("  SELECT  type text\n");
}

/* Progress callback used during connect / scan / keygen: redraw + allow cancel. */
static bool on_progress(const char *msg, void *user) {
    (void)user;
    static char shown[96] = "";
    if (!aptMainLoop()) return false;
    hidScanInput();
    if (hidKeysDown() & KEY_B) return false;
    if (strcmp(shown, msg) != 0) {
        snprintf(shown, sizeof(shown), "%s", msg);
        char line[128];
        snprintf(line, sizeof(line), "%s\n (B to cancel)", msg);
        draw_top(line);
    }
    present();
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
    return ok && inet_aton(tv_ip, &a);
}

/* ---------- actions ---------- */
static void send_key(int code, const char *name) {
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "input keyevent %d", code);
    adb_queue(cmd);
    snprintf(last_action, sizeof(last_action), "%s", name);
}

static void send_longpress(int code, const char *name) {
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "input keyevent --longpress %d", code);
    adb_queue(cmd);
    snprintf(last_action, sizeof(last_action), "%s (long)", name);
}

static bool connect_tv(void) {
    if (!tv_ip[0]) return false;
    char msg[64];
    snprintf(msg, sizeof(msg), "Connecting to %s...", tv_ip);
    on_progress(msg, NULL);
    return adb_connect(tv_ip, ADB_PORT, AUTH_WAIT_MS, on_progress, NULL) == 0;
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
    return true;
}

/* Find the TV: scan the 3DS's /24 subnet for port 5555, otherwise ask. */
static void find_tv(void) {
    struct in_addr me;
    me.s_addr = (in_addr_t)gethostid();
    if (me.s_addr != 0) {
        char found[8][16];
        int n = adb_scan(inet_ntoa(me), ADB_PORT, found, 8, on_progress, NULL);
        if (n > 0) {
            snprintf(tv_ip, sizeof(tv_ip), "%s", found[0]);
            save_ip();
            return;
        }
    }
    if (prompt_ip()) save_ip();
}

/* `input text` treats %s as a space; quote everything else for sh. */
static void type_text(void) {
    SwkbdState kb;
    char text[121] = {0};
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 120);
    swkbdSetHintText(&kb, "Text to type on the TV");
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, text, sizeof(text)) != SWKBD_BUTTON_CONFIRM) return;

    char cmd[256] = "input text '";
    size_t o = strlen(cmd);
    for (const char *p = text; *p && o < sizeof(cmd) - 8; p++) {
        if (*p == ' ')       { memcpy(cmd + o, "%s", 2); o += 2; }
        else if (*p == '\'') { memcpy(cmd + o, "'\\''", 4); o += 4; }
        else if (*p == '%')  { memcpy(cmd + o, "\\%", 2); o += 2; }
        else                 cmd[o++] = *p;
    }
    cmd[o++] = '\'';
    cmd[o] = 0;
    adb_queue(cmd);
    snprintf(last_action, sizeof(last_action), "typed \"%.30s\"", text);
}

/* ---------- main ---------- */
int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bot);
    osSetSpeedupEnable(true);         /* faster key generation on New 3DS */
    mkdir("sdmc:/3ds", 0777);
    mkdir(APP_DIR, 0777);

    draw_bottom();
    on_progress("Starting Wi-Fi...", NULL);
    soc_buf = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!soc_buf || R_FAILED(socInit(soc_buf, SOC_BUFFERSIZE))) {
        draw_top("Network init failed. Is Wi-Fi on?\n Press START to exit.");
        while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; present(); }
        gfxExit();
        return 0;
    }

    if (adb_init(KEY_PATH, on_progress, NULL) != 0) {
        draw_top(adb_last_error());
    } else {
        if (!load_ip()) find_tv();
        connect_tv();
    }

    hidSetRepeatParameters(15, 5);    /* frames: initial delay, repeat rate */
    draw_top(NULL);
    draw_bottom();

    int a_frames = 0;
    bool a_long_sent = false;
    int pressed_btn = -1, pressed_timer = 0;
    bool was_connected = adb_is_connected();

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown(), rep = hidKeysDownRepeat();
        u32 held = hidKeysHeld(), up = hidKeysUp();
        bool connected = adb_is_connected();
        bool dirty = false, quit = false;

        if (connected) {
            if (rep & KEY_UP)    { send_key(K_UP, "up");       dirty = true; }
            if (rep & KEY_DOWN)  { send_key(K_DOWN, "down");   dirty = true; }
            if (rep & KEY_LEFT)  { send_key(K_LEFT, "left");   dirty = true; }
            if (rep & KEY_RIGHT) { send_key(K_RIGHT, "right"); dirty = true; }

            if (held & KEY_A) {
                if (++a_frames == LONGPRESS_FRAMES && !a_long_sent) {
                    send_longpress(K_SELECT, "select"); a_long_sent = true; dirty = true;
                }
            }
            if (up & KEY_A) {
                if (!a_long_sent && a_frames > 0) { send_key(K_SELECT, "select"); dirty = true; }
                a_frames = 0; a_long_sent = false;
            }
            if (down & KEY_B)     { send_key(K_BACK, "back");       dirty = true; }
            if (down & KEY_X)     { send_key(K_HOME, "home");       dirty = true; }
            if (down & KEY_Y)     { send_key(K_MENU, "menu");       dirty = true; }
            if (down & KEY_START) { send_key(K_PLAYPAUSE, "play/pause"); dirty = true; }
            if (rep & KEY_L)      { send_key(K_REWIND, "rewind");   dirty = true; }
            if (rep & KEY_R)      { send_key(K_FF, "fast-forward"); dirty = true; }
            if (rep & KEY_ZL)     { send_key(K_VOLDOWN, "vol -");   dirty = true; }
            if (rep & KEY_ZR)     { send_key(K_VOLUP, "vol +");     dirty = true; }
            if (down & KEY_CSTICK_LEFT)  { send_key(K_PREV, "previous"); dirty = true; }
            if (down & KEY_CSTICK_RIGHT) { send_key(K_NEXT, "next");     dirty = true; }
            if (down & KEY_SELECT) { type_text(); dirty = true; }
        } else if (down & KEY_A) {
            connect_tv();
            a_frames = 0;           /* don't turn this press into a select */
            dirty = true;
        }

        if (down & KEY_TOUCH) {
            touchPosition t;
            hidTouchRead(&t);
            int col = t.px / 8, row = t.py / 8;
            for (int i = 0; i < NBUTTONS; i++) {
                const Button *b = &buttons[i];
                if (row < b->row || row >= b->row + BH || col < b->col || col >= b->col + BW) continue;
                draw_button(b, true);
                pressed_btn = i; pressed_timer = 8;
                switch (b->type) {
                    case ACT_KEY:   if (connected) send_key(b->key, b->label); break;
                    case ACT_SHELL: if (connected) { adb_queue(b->shell); snprintf(last_action, sizeof(last_action), "open %s", b->label); } break;
                    case ACT_TYPE:  if (connected) type_text(); break;
                    case ACT_SETTV:
                        adb_disconnect();
                        if (prompt_ip()) save_ip();
                        connect_tv();
                        break;
                    case ACT_EXIT:  quit = true; break;
                }
                dirty = true;
                break;
            }
        }
        if (quit) break;

        if (pressed_btn >= 0 && --pressed_timer <= 0) {
            draw_button(&buttons[pressed_btn], false);
            pressed_btn = -1;
        }

        adb_poll();
        if (adb_is_connected() != was_connected) { was_connected = adb_is_connected(); dirty = true; }
        if (dirty) draw_top(NULL);
        present();
    }

    adb_shutdown();
    socExit();
    free(soc_buf);
    gfxExit();
    return 0;
}
