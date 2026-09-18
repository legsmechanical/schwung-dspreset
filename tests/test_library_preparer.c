#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../src/dsp/dspreset/library_preparer.h"

int main(int argc, char **argv) {
    char error[128] = {0};
    char preset[1024];
    struct stat st;
    ds_library_prepare_result_t result = {0};
    assert(argc == 3);
    assert(ds_library_prepare_archive(argv[1], argv[2], &result, error, sizeof(error)) == 0);
    assert(result.preset_files == 1);
    assert(result.sample_files > 0);
    assert(snprintf(preset, sizeof(preset), "%s/Capture GO-TO Bass/Capture GO-TO Bass.dspreset",
                    argv[2]) < (int)sizeof(preset));
    assert(stat(preset, &st) == 0 && st.st_size > 0);
    printf("prepared %u files (%u samples)\n", result.extracted_files, result.sample_files);
    return 0;
}
