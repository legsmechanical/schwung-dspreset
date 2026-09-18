/* Writes the params and hierarchy a host sees for a loaded preset (the
 * controls test preset, so knob pages exist) to $TEST_TMP/contract.json, for
 * tests/test_pages.mjs to lay out with the hosts' own page planner. */
#include "test_support.h"

static const char *PRESET =
    "<DecentSampler><ui><tab><labeled-knob label=\"Tone\" minValue=\"0\" maxValue=\"1\" value=\"1\">"
    "<binding type=\"amp\" level=\"instrument\" parameter=\"AMP_VOLUME\"/></labeled-knob></tab></ui>"
    "<groups><group><sample path=\"t.wav\" rootNote=\"60\"/></group></groups></DecentSampler>";

int main(void) {
    const char *tmp = getenv("TEST_TMP");
    static char hierarchy[65536], params[65536];
    char mod[512], path[1024];
    plugin_t p;
    FILE *f;
    CHECK(tmp);
    snprintf(mod, sizeof(mod), "%s/pages-module", tmp);
    snprintf(path, sizeof(path), "mkdir -p '%s/instruments/P'", mod); CHECK(system(path) == 0);
    snprintf(path, sizeof(path), "%s/instruments/P/t.wav", mod); write_wav24(path, 44100, 1, 1000, -1, -1);
    snprintf(path, sizeof(path), "%s/instruments/P/p.dspreset", mod); write_text(path, PRESET);
    plugin_open_in(&p, mod);
    usleep(700000);
    for (int i = 0; i < 300 && plugin_uint(&p, "load_count") == 0; ++i) usleep(10000);
    CHECK(plugin_uint(&p, "load_count") == 1);
    plugin_get(&p, "ui_hierarchy", hierarchy, sizeof(hierarchy));
    plugin_get(&p, "chain_params", params, sizeof(params));
    plugin_close(&p);
    snprintf(path, sizeof(path), "%s/contract.json", tmp);
    CHECK((f = fopen(path, "w")) != NULL);
    fprintf(f, "{\"hierarchy\":%s,\"chainParams\":%s}\n", hierarchy, params);
    fclose(f);
    puts("pages contract written");
    return 0;
}
