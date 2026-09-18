/* Bank and preset selection through the real plugin, the way both hosts drive
 * it: Banks is an items list (bank_list / bank), Presets a browser
 * (preset / preset_count / preset_name). */
#include "test_support.h"

static void sh(const char *fmt, const char *dir) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), fmt, dir, dir, dir, dir, dir, dir);
    CHECK(system(cmd) == 0);
}

static void preset(const char *path, const char *sample) {
    char xml[512];
    snprintf(xml, sizeof(xml), "<DecentSampler><groups><group><sample path=\"%s\" rootNote=\"60\"/></group></groups></DecentSampler>", sample);
    write_text(path, xml);
}

static void wait_ms(int ms) { usleep((useconds_t)ms * 1000); }

/* Waits until the worker has nothing pending; returns the status line. */
static const char *settle(plugin_t *p, char *status, int n) {
    for (int i = 0; i < 1000; ++i) {
        wait_ms(10);
        plugin_get(p, "status", status, n);
        if (plugin_uint(p, "loading") == 0 && strncmp(status, "Loading", 7) && strncmp(status, "Unpacking", 9) && i > 30) break;
    }
    return status;
}

static void expect_name(plugin_t *p, const char *want) {
    char name[256];
    plugin_get(p, "preset_name", name, sizeof(name));
    if (strcmp(name, want)) { fprintf(stderr, "FAIL preset_name '%s', want '%s'\n", name, want); exit(1); }
}

int main(void) {
    const char *tmp = getenv("TEST_TMP");
    char mod[512], path[1024], value[4096], status[256], state[2048];
    plugin_t p, q;
    unsigned loads;
    CHECK(tmp);
    snprintf(mod, sizeof(mod), "%s/banks-module", tmp);
    /* instruments/: a folder bank, a .dslibrary not yet unpacked, a loose
     * preset, a folder with no presets (not a bank) and a slow big one. */
    sh("rm -rf '%s/banks-module' && mkdir -p '%s/banks-module/instruments/Alpha/Sub' "
       "'%s/banks-module/instruments/Empty' '%s/banks-module/pkg/Beta' '%s/banks-module/instruments/Zulu Big'", tmp);
    snprintf(path, sizeof(path), "%s/instruments/Alpha/s.wav", mod);        write_wav24(path, 44100, 1, 20000, -1, -1);
    snprintf(path, sizeof(path), "%s/instruments/Alpha/10 - Ten.dspreset", mod); preset(path, "s.wav");
    snprintf(path, sizeof(path), "%s/instruments/Alpha/2 - Two.dspreset", mod);  preset(path, "s.wav");
    snprintf(path, sizeof(path), "%s/instruments/Alpha/Sub/3 - Three.dspreset", mod); preset(path, "../s.wav");
    snprintf(path, sizeof(path), "%s/instruments/Solo.dspreset", mod);      preset(path, "Alpha/s.wav");
    snprintf(path, sizeof(path), "%s/pkg/Beta/b.wav", mod);                 write_wav24(path, 44100, 1, 20000, -1, -1);
    snprintf(path, sizeof(path), "%s/pkg/Beta/Beta One.dspreset", mod);     preset(path, "b.wav");
    sh("cd '%s/banks-module/pkg' && python3 -m zipfile -c '%s/banks-module/instruments/Beta.dslibrary' Beta", tmp);
    /* Zulu Big: 300 files, so a load takes long enough to be interrupted */
    {
        char xml[40000]; int k = snprintf(xml, sizeof(xml), "<DecentSampler><groups><group>");
        for (int i = 0; i < 300; ++i) {
            snprintf(path, sizeof(path), "%s/instruments/Zulu Big/z%d.wav", mod, i); write_wav24(path, 44100, 1, 40000, -1, -1);
            k += snprintf(xml + k, sizeof(xml) - (size_t)k, "<sample path=\"z%d.wav\" rootNote=\"%d\"/>", i, i % 128);
        }
        snprintf(xml + k, sizeof(xml) - (size_t)k, "</group></groups></DecentSampler>");
        snprintf(path, sizeof(path), "%s/instruments/Zulu Big/Big.dspreset", mod); write_text(path, xml);
    }

    plugin_open_in(&p, mod);
    wait_ms(600);                                           /* past the wait for a restored state */
    settle(&p, status, sizeof(status));

    /* the bank list: natural order, the empty folder left out, a .dslibrary listed */
    plugin_get(&p, "bank_list", value, sizeof(value));
    printf("  banks: %s\n", value);
    CHECK(!strcmp(value, "[{\"label\":\"Alpha\",\"index\":0},{\"label\":\"Beta\",\"index\":1},"
                         "{\"label\":\"Solo\",\"index\":2},{\"label\":\"Zulu Big\",\"index\":3}]"));

    /* a fresh instance with nothing restored starts on the first playable bank */
    CHECK(plugin_uint(&p, "bank") == 0 && plugin_uint(&p, "preset_count") == 3);
    expect_name(&p, "2 - Two");
    CHECK(!strcmp(status, "2 - Two.dspreset: 1 zones"));
    CHECK(plugin_uint(&p, "load_count") == 1);

    /* scrolling: names answer at once and nothing loads on the way past —
     * this is exactly what dAVEBOx's name scan does to every index */
    loads = plugin_uint(&p, "load_count");
    for (int round = 0; round < 3; ++round)
        for (int i = 0; i < 3; ++i) {
            char idx[8]; snprintf(idx, sizeof(idx), "%d", i);
            p.api->set_param(p.instance, "preset", idx);
            expect_name(&p, i == 0 ? "2 - Two" : i == 1 ? "10 - Ten" : "3 - Three");   /* by path: Sub/ sorts last */
            wait_ms(20);
        }
    p.api->set_param(p.instance, "preset", "0");           /* the scan puts it back */
    settle(&p, status, sizeof(status));
    CHECK(plugin_uint(&p, "load_count") == loads);

    /* a pick that stays put loads */
    p.api->set_param(p.instance, "preset", "1");
    settle(&p, status, sizeof(status));
    CHECK(!strcmp(status, "10 - Ten.dspreset: 1 zones") && plugin_uint(&p, "load_count") == loads + 1);

    /* choosing the .dslibrary bank unpacks it, then plays its first preset */
    p.api->set_param(p.instance, "bank", "1");
    settle(&p, status, sizeof(status));
    printf("  after choosing Beta: %s\n", status);
    CHECK(!strcmp(status, "Beta One.dspreset: 1 zones"));
    expect_name(&p, "Beta One");
    snprintf(path, sizeof(path), "%s/instruments/Beta.dslibrary.unpacked", mod);
    { struct stat st; CHECK(!stat(path, &st) && S_ISDIR(st.st_mode)); }
    plugin_get(&p, "bank_list", value, sizeof(value));
    CHECK(!strstr(value, "unpacked"));                     /* its folder is not a second bank */

    /* moving off a big load before it finishes abandons it */
    loads = plugin_uint(&p, "load_count");
    p.api->set_param(p.instance, "bank", "3");
    for (int i = 0; i < 200 && !plugin_uint(&p, "loading"); ++i) wait_ms(5);
    CHECK(plugin_uint(&p, "loading"));
    p.api->set_param(p.instance, "bank", "2");
    settle(&p, status, sizeof(status));
    CHECK(!strcmp(status, "Solo.dspreset: 1 zones") && plugin_uint(&p, "load_count") == loads + 1);

    /* state round-trips into a NEW instance: same bank, same preset, one load */
    p.api->set_param(p.instance, "bank", "0");
    p.api->set_param(p.instance, "preset", "2");
    settle(&p, status, sizeof(status));
    plugin_get(&p, "state", state, sizeof(state));
    printf("  state: %s\n", state);
    CHECK(strstr(state, "Sub/3 - Three.dspreset"));
    plugin_open_in(&q, mod);
    q.api->set_param(q.instance, "state", state);
    settle(&q, status, sizeof(status));
    CHECK(!strcmp(status, "3 - Three.dspreset: 1 zones"));
    CHECK(plugin_uint(&q, "bank") == 0 && plugin_uint(&q, "preset") == 2);
    CHECK(plugin_uint(&q, "load_count") == 1);             /* no detour through the first preset */
    plugin_close(&q);

    plugin_close(&p);
    puts("banks test passed");
    return 0;
}
