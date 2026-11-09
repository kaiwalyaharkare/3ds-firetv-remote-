/* Host-side test of adb.c against tests/mock_adbd.py */
#include "../3ds/source/adb.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static bool prog(const char *m, void *u) { (void)u; static char last[128]; if (strcmp(last,m)) { printf("  [%s]\n", m); snprintf(last,sizeof last,"%s",m);} return true; }
static int run(const char *ip, int port) {
    if (adb_connect(ip, port, 5000, prog, NULL) != 0) { printf("connect failed: %s\n", adb_last_error()); return 1; }
    printf("connected to %s\n", adb_device_name());
    const char *cmds[] = {"input keyevent 19","input keyevent 19","input keyevent 20","input keyevent --longpress 23","input text 'hi%sthere'","input keyevent 4"};
    for (int i = 0; i < 6; i++) adb_queue(cmds[i]);
    for (int i = 0; i < 100; i++) { if (!adb_poll()) { printf("poll failed: %s\n", adb_last_error()); return 1; } usleep(10000); }
    adb_disconnect();
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (adb_init(argv[1], prog, NULL) != 0) { printf("init failed: %s\n", adb_last_error()); return 1; }
    int port = 5557;
    printf("-- first connection (new key)\n");  if (run("127.0.0.77", port)) return 1;
    printf("-- second connection (known key)\n"); if (run("127.0.0.77", port)) return 1;
    char found[8][16];
    int n = adb_scan("127.0.0.5", port, found, 8, prog, NULL);
    printf("-- scan found %d:", n); for (int i = 0; i < n; i++) printf(" %s", found[i]); printf("\n");
    adb_shutdown();
    return 0;
}
