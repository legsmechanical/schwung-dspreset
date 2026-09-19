/* The hierarchy both hosts read: a preset browser on root (list/count/name)
 * and a Banks level (items/select) that returns to it. */
#include "test_support.h"

int main(void) {
    char value[4096];
    plugin_t p;
    plugin_open_in(&p, getenv("TEST_TMP"));
    plugin_get(&p, "ui_hierarchy", value, sizeof(value));
    CHECK(strstr(value, "\"list_param\":\"preset\"") && strstr(value, "\"count_param\":\"preset_count\""));
    CHECK(strstr(value, "\"name_param\":\"preset_name\""));
    CHECK(strstr(value, "\"items_param\":\"bank_list\"") && strstr(value, "\"select_param\":\"bank\""));
    CHECK(strstr(value, "\"navigate_to\":\"presets\"") && strstr(value, "\"level\":\"banks\""));
    CHECK(!strstr(value, "filepath"));
    plugin_get(&p, "chain_params", value, sizeof(value));
    CHECK(strstr(value, "\"key\":\"preset\"") && strstr(value, "\"key\":\"bank\"") && strstr(value, "\"key\":\"gain\""));
    plugin_close(&p);
    puts("plugin metadata test passed");
    return 0;
}
