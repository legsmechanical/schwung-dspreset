/* DSLibraryInfo.xml: the library's own name for its bank, and <presetMenu>
 * reordering its presets without moving them — the guide's own example:
 * top-level entries (menus and presets it does not mention) alphabetical, a
 * menu in the order written, nested menus as "Pads / Analog / Drift", a
 * missing file skipped, an empty menu dropped; the file found inside a
 * .dslibrary's single folder; a library without one unchanged; and the real
 * plugin listing and loading what the menu says. */
#include "test_support.h"

#include "../src/dsp/dspreset/catalog.h"

static void sh(const char *fmt, const char *dir) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), fmt, dir, dir, dir, dir, dir, dir);
    CHECK(system(cmd) == 0);
}

static void preset(const char *root, const char *rel) {
    char path[1024], *slash;
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    write_text(path, "<DecentSampler><groups><group><sample path=\"s.wav\" rootNote=\"60\"/></group></groups></DecentSampler>");
    slash = strrchr(path, '/');
    snprintf(slash, sizeof(path) - (size_t)(slash - path), "/s.wav");
    write_wav24(path, 44100, 1, 2000, -1, -1);
}

static const char *INFO =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<DecentSamplerLibraryInfo name=\"My Sample Library\" version=\"1.2.0\">\n"
    "  <presetMenu>\n"
    "    <menu name=\"Pads\">\n"
    "      <preset file=\"Presets/Pads/WarmPad.dspreset\"/>\n"
    "      <preset file=\"Presets/Pads/GlassPad.dspreset\"/>\n"
    "      <menu name=\"Analog\">\n"
    "        <preset file=\"Presets/Pads/Analog/Drift.dspreset\"/>\n"
    "      </menu>\n"
    "      <preset file=\"Presets/Pads/Gone.dspreset\"/>\n"
    "    </menu>\n"
    "    <menu name=\"Leads\">\n"
    "      <preset file=\"Presets\\Leads\\Saw.dspreset\"/>\n"
    "    </menu>\n"
    "    <menu name=\"Empty\"><preset file=\"Presets/Nowhere.dspreset\"/></menu>\n"
    "  </presetMenu>\n"
    "</DecentSamplerLibraryInfo>\n";

static const char *WANT[] = {"Bass", "Leads / Saw", "Pads / WarmPad", "Pads / GlassPad", "Pads / Analog / Drift"};

static void layout(const char *root) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s/Presets/Pads/Analog' '%s/Presets/Leads' '%s/Bass'", root, root, root);
    CHECK(system(cmd) == 0);
    preset(root, "Presets/Pads/WarmPad.dspreset");
    preset(root, "Presets/Pads/GlassPad.dspreset");
    preset(root, "Presets/Pads/Analog/Drift.dspreset");
    preset(root, "Presets/Leads/Saw.dspreset");
    preset(root, "Bass/Bass.dspreset");
}

int main(void) {
    const char *tmp = getenv("TEST_TMP");
    char mod[512], path[1024], value[4096];
    ds_catalog_t *c;
    CHECK(tmp);
    snprintf(mod, sizeof(mod), "%s/menu-module", tmp);
    sh("rm -rf '%s/menu-module' && mkdir -p '%s/menu-module/instruments/Lib' '%s/menu-module/instruments/Plain' '%s/menu-module/pkg/Packed'", tmp);
    snprintf(path, sizeof(path), "%s/instruments/Lib", mod); layout(path);
    snprintf(path, sizeof(path), "%s/instruments/Lib/DSLibraryInfo.xml", mod); write_text(path, INFO);
    snprintf(path, sizeof(path), "%s/instruments/Plain", mod); layout(path);           /* no info file */
    /* a .dslibrary, its info file inside the one folder it holds, already unpacked */
    snprintf(path, sizeof(path), "%s/instruments/Packed.dslibrary.unpacked/Packed", mod); layout(path);
    snprintf(path, sizeof(path), "%s/instruments/Packed.dslibrary.unpacked/Packed/DSLibraryInfo.xml", mod); write_text(path, INFO);
    sh("cd '%s/menu-module/pkg' && python3 -m zipfile -c '%s/menu-module/instruments/Packed.dslibrary' Packed", tmp);

    snprintf(path, sizeof(path), "%s/instruments", mod);
    CHECK((c = ds_catalog_scan(path)) != NULL);
    CHECK(c->bank_count == 3);
    {
        int seen = 0;
        for (unsigned i = 0; i < c->bank_count; ++i) {
            const ds_bank_t *b = &c->banks[i];
            printf("  bank \"%s\":", b->name);
            for (unsigned k = 0; k < b->preset_count; ++k) printf(" [%s]", b->preset_names[k]);
            printf("\n");
            if (!strcmp(b->name, "Plain")) {                       /* no info file: the plain, natural order */
                CHECK(b->preset_count == 5 && !strcmp(b->preset_names[0], "Bass") && !strcmp(b->preset_names[1], "Saw"));
                seen |= 1;
                continue;
            }
            CHECK(!strcmp(b->name, "My Sample Library"));
            CHECK(b->preset_count == 5);
            for (unsigned k = 0; k < 5; ++k) CHECK(!strcmp(b->preset_names[k], WANT[k]));
            CHECK(strstr(b->preset_paths[1], "Presets/Leads/Saw.dspreset"));
            CHECK(strstr(b->preset_paths[4], "Presets/Pads/Analog/Drift.dspreset"));
            seen |= b->kind == DS_BANK_DSLIBRARY ? 2 : 4;
        }
        CHECK(seen == 7);
    }
    /* renaming a library alone is a change the catalog reports (so the Move's
     * list follows): "Plain" -> "Plainer" keeps its place, so only the name differs */
    {
        ds_catalog_t *again;
        snprintf(path, sizeof(path), "%s/instruments/Plain/DSLibraryInfo.xml", mod);
        write_text(path, "<DecentSamplerLibraryInfo name=\"Plainer\"/>");
        snprintf(path, sizeof(path), "%s/instruments", mod);
        CHECK((again = ds_catalog_scan(path)) != NULL);
        CHECK(!strcmp(again->banks[2].name, "Plainer") && !strcmp(again->banks[2].path, c->banks[2].path));
        CHECK(!ds_catalog_equal(c, again));
        ds_catalog_free(again);
        snprintf(path, sizeof(path), "rm '%s/instruments/Plain/DSLibraryInfo.xml'", mod); CHECK(system(path) == 0);
        snprintf(path, sizeof(path), "%s/instruments", mod);
        CHECK((again = ds_catalog_scan(path)) != NULL);
        CHECK(ds_catalog_equal(c, again));
        ds_catalog_free(again);
    }
    ds_catalog_free(c);

    /* the real plugin: the bank by the library's name, the presets in menu order */
    {
        plugin_t p;
        char status[256];
        plugin_open_in(&p, mod);
        for (int i = 0; i < 300; ++i) { usleep(10000); if (i > 40 && !plugin_uint(&p, "is_loading")) break; }
        plugin_get(&p, "bank_list", value, sizeof(value));
        printf("  banks: %s\n", value);
        CHECK(strstr(value, "My Sample Library"));
        for (int b = 0; b < 3; ++b) {
            char key[16];
            snprintf(key, sizeof(key), "%d", b);
            p.api->set_param(p.instance, "bank", key);
            for (int i = 0; i < 300; ++i) { usleep(10000); if (i > 20 && !plugin_uint(&p, "is_loading")) break; }
            plugin_get(&p, "preset_count", value, sizeof(value));
            p.api->set_param(p.instance, "preset", "1");
            for (int i = 0; i < 300; ++i) { usleep(10000); if (i > 20 && !plugin_uint(&p, "is_loading")) break; }
            plugin_get(&p, "preset_name", value, sizeof(value));
            plugin_get(&p, "status", status, sizeof(status));
            printf("  bank %d, preset 1: %s (%s)\n", b, value, status);
            CHECK(!strcmp(value, b < 2 ? "Leads / Saw" : "Saw"));
            CHECK(!strncmp(status, "Saw.dspreset", 12));
        }
        plugin_close(&p);
    }

    puts("library menu test passed");
    return 0;
}
