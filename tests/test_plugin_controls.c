/* The preset's controls as the HOST sees them: listed as params and knobs,
 * moved by set_param, saved and restored with the project, and an is_loading
 * edge around a preset change so both hosts' pages re-read the new controls. */
#include "test_support.h"

static const char *PRESET =
    "<DecentSampler>\n"
    "<ui><tab>\n"
    "  <labeled-knob label=\"Close\" minValue=\"0\" maxValue=\"1\" value=\"1\">\n"
    "    <binding type=\"amp\" level=\"group\" position=\"0\" parameter=\"AMP_VOLUME\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"1\"/>\n"
    "  </labeled-knob>\n"
    "  <control minValue=\"0\" maxValue=\"100\" value=\"100\"><binding type=\"amp\" level=\"tag\" identifier=\"mic2\" parameter=\"AMP_VOLUME\" factor=\"0.01\"/></control>\n"
    "  <labeled-knob label=\"\" minValue=\"0\" maxValue=\"10\" value=\"3.5\" type=\"percent\">\n"
    "    <binding type=\"amp\" level=\"instrument\" position=\"0\" parameter=\"ENV_RELEASE\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"10\"/>\n"
    "  </labeled-knob>\n"
    "  <button value=\"0\" parameterName=\"Layer\">\n"
    "    <state name=\"A\"><binding type=\"general\" level=\"group\" position=\"2\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"false\"/>\n"
    "                    <binding type=\"general\" level=\"group\" position=\"3\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"true\"/></state>\n"
    "    <state name=\"B\"><binding type=\"general\" level=\"group\" position=\"2\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"true\"/>\n"
    "                    <binding type=\"general\" level=\"group\" position=\"3\" parameter=\"ENABLED\" translation=\"fixed_value\" translationValue=\"false\"/></state>\n"
    "  </button>\n"
    "  <menu value=\"1\">\n"
    "    <option name=\"Up\"><binding type=\"amp\" level=\"instrument\" parameter=\"GLOBAL_TUNING\" translation=\"fixed_value\" translationValue=\"0\"/></option>\n"
    "    <option name=\"Down\"><binding type=\"amp\" level=\"instrument\" parameter=\"GLOBAL_TUNING\" translation=\"fixed_value\" translationValue=\"-12\"/></option>\n"
    "  </menu>\n"
    "  <labeled-knob minValue=\"1\" maxValue=\"10\" value=\"10\">\n"
    "    <binding type=\"effect\" level=\"instrument\" position=\"0\" parameter=\"FX_FILTER_FREQUENCY\" translation=\"table\" translationTable=\"0,33;5,1000;10,22000\"/>\n"
    "  </labeled-knob>\n"
    "</tab></ui>\n"
    "<groups attack=\"0\" release=\"0.2\">\n"
    "  <group name=\"Close\" tags=\"mic1\"><sample path=\"a.wav\" rootNote=\"60\" loNote=\"60\" hiNote=\"60\"/></group>\n"
    "  <group name=\"Room\" tags=\"mic2\"><sample path=\"b.wav\" rootNote=\"60\" loNote=\"60\" hiNote=\"60\"/></group>\n"
    "  <group name=\"Layer B\" enabled=\"false\"><sample path=\"c.wav\" rootNote=\"62\" loNote=\"62\" hiNote=\"62\"/></group>\n"
    "  <group name=\"Layer A\"><sample path=\"d.wav\" rootNote=\"62\" loNote=\"62\" hiNote=\"62\"/></group>\n"
    "</groups>\n"
    "<effects><effect type=\"lowpass\" frequency=\"22000\"/></effects>\n"
    "<midi><cc number=\"1\"><binding level=\"ui\" type=\"control\" parameter=\"VALUE\" position=\"0\" translation=\"linear\" translationOutputMin=\"0\" translationOutputMax=\"1\"/></cc></midi>\n"
    "</DecentSampler>\n";


static void render(plugin_t *p, int blocks) { int16_t out[256]; while (blocks--) plugin_render(p, out); }

static void settle(plugin_t *p) {
    for (int i = 0; i < 500; ++i) { usleep(10000); if (i > 20 && !plugin_uint(p, "is_loading")) break; }
}

int main(void) {
    const char *tmp = getenv("TEST_TMP");
    char mod[512], path[2048], value[8192], state[2048];
    plugin_t p, q;
    CHECK(tmp);
    snprintf(mod, sizeof(mod), "%s/ctl-module", tmp);
    snprintf(path, sizeof(path), "rm -rf '%s' && mkdir -p '%s/instruments/Ctl'", mod, mod); CHECK(system(path) == 0);
    for (const char *f = "abcd"; *f; ++f) { snprintf(path, sizeof(path), "%s/instruments/Ctl/%c.wav", mod, *f); write_wav24(path, 44100, 1, 30000, -1, -1); }
    snprintf(path, sizeof(path), "%s/instruments/Ctl/1 One.dspreset", mod); write_text(path, PRESET);
    snprintf(path, sizeof(path), "%s/instruments/Ctl/2 Two.dspreset", mod); write_text(path, PRESET);

    plugin_open_in(&p, mod);
    usleep(600000);
    settle(&p);
    CHECK(plugin_uint(&p, "load_count") == 1 && !plugin_uint(&p, "is_loading"));

    plugin_get(&p, "ui_hierarchy", value, sizeof(value));
    CHECK(strstr(value, "\"knobs\":[\"ctl_0\",\"ctl_1\",\"ctl_2\",\"ctl_3\",\"ctl_4\",\"ctl_5\",\"gain\"]"));
    plugin_get(&p, "chain_params", value, sizeof(value));
    CHECK(strstr(value, "{\"key\":\"ctl_0\",\"name\":\"Close\",\"type\":\"float\",\"min\":0,\"max\":1"));
    CHECK(strstr(value, "{\"key\":\"ctl_3\",\"name\":\"Layer\",\"type\":\"enum\",\"options\":[\"A\",\"B\"]"));
    CHECK(strstr(value, "\"name\":\"Tune\",\"type\":\"enum\",\"options\":[\"Up\",\"Down\"]"));

    /* a move from the host reads back at once, and lands on the audio thread */
    p.api->set_param(p.instance, "ctl_0", "0.25");
    plugin_get(&p, "ctl_0", value, sizeof(value));
    CHECK(!strcmp(value, "0.2500"));
    p.api->set_param(p.instance, "ctl_3", "1");
    render(&p, 2);
    plugin_get(&p, "ctl_3", value, sizeof(value));
    CHECK(!strcmp(value, "1"));

    /* saved with the project, restored into a new instance before it is heard */
    plugin_get(&p, "state", state, sizeof(state));
    printf("  state: %s\n", strstr(state, "\"controls\""));
    CHECK(strstr(state, "\"controls\":\"0.25;100;3.5;1;0;10\""));
    plugin_open_in(&q, mod);
    q.api->set_param(q.instance, "state", state);
    settle(&q);
    plugin_get(&q, "ctl_0", value, sizeof(value));
    CHECK(!strcmp(value, "0.2500"));
    plugin_get(&q, "ctl_3", value, sizeof(value));
    CHECK(!strcmp(value, "1"));
    CHECK(plugin_uint(&q, "load_count") == 1);
    plugin_close(&q);

    /* a new preset: is_loading goes up with the pick and down once it plays,
     * and the new preset's controls start at ITS values, not the last one's */
    p.api->set_param(p.instance, "preset", "1");
    CHECK(plugin_uint(&p, "is_loading") == 1);
    settle(&p);
    CHECK(plugin_uint(&p, "load_count") == 2 && !plugin_uint(&p, "is_loading"));
    plugin_get(&p, "ctl_0", value, sizeof(value));
    CHECK(!strcmp(value, "1.0000"));

    plugin_close(&p);
    puts("plugin controls test passed");
    return 0;
}
