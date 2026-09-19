/* The instruments folder is not walked on a timer (a walk of every file every
 * 5 s stalled the Move's audio on the SD card): a library copied in while the
 * module runs appears once someone looks at the bank list, and one removed
 * disappears; nothing is re-read while the folder stays as it was. */
#include "test_support.h"

static void settle(plugin_t *p) {
    for (int i = 0; i < 300; ++i) { usleep(10000); if (i > 20 && !plugin_uint(p, "is_loading")) break; }
}

/* Reads the bank list until it contains (or lacks) `name`, up to ~3 s. */
static int banks_show(plugin_t *p, const char *name, int want) {
    char value[4096];
    for (int i = 0; i < 60; ++i) {
        plugin_get(p, "bank_list", value, sizeof(value));
        if ((strstr(value, name) != NULL) == want) return 1;
        usleep(50000);
    }
    return 0;
}

int main(void) {
    const char *tmp = getenv("TEST_TMP");
    char mod[512], path[1024], cmd[2048];
    plugin_t p;
    CHECK(tmp);
    snprintf(mod, sizeof(mod), "%s/changes-module", tmp);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/instruments/First'", mod, mod); CHECK(system(cmd) == 0);
    snprintf(path, sizeof(path), "%s/instruments/First/s.wav", mod); write_wav24(path, 44100, 1, 2000, -1, -1);
    snprintf(path, sizeof(path), "%s/instruments/First/One.dspreset", mod);
    write_text(path, "<DecentSampler><groups><group><sample path=\"s.wav\" rootNote=\"60\"/></group></groups></DecentSampler>");
    plugin_open_in(&p, mod);
    usleep(600000);
    settle(&p);
    CHECK(banks_show(&p, "First", 1) && banks_show(&p, "Second", 0));

    /* copied in while it runs */
    snprintf(cmd, sizeof(cmd), "cp -R '%s/instruments/First' '%s/instruments/Second'", mod, mod); CHECK(system(cmd) == 0);
    CHECK(banks_show(&p, "Second", 1));
    /* and taken away */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s/instruments/Second'", mod); CHECK(system(cmd) == 0);
    CHECK(banks_show(&p, "Second", 0));

    plugin_close(&p);
    puts("folder changes test passed");
    return 0;
}
