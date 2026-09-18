#include <assert.h>
#include <stdio.h>

#include "../src/dsp/dspreset/library_input.h"

int main(void) {
    assert(ds_classify_library_input("Bass.dslibrary", 0) == DS_LIBRARY_INPUT_DSLIBRARY_ARCHIVE);
    assert(ds_classify_library_input("BASS.DSLIBRARY", 0) == DS_LIBRARY_INPUT_DSLIBRARY_ARCHIVE);
    assert(ds_classify_library_input("Bass.dspreset", 0) == DS_LIBRARY_INPUT_PRESET_FILE);
    assert(ds_classify_library_input("Bass", 1) == DS_LIBRARY_INPUT_DIRECTORY);
    assert(ds_classify_library_input("Bass.sfz", 0) == DS_LIBRARY_INPUT_UNSUPPORTED);
    assert(ds_library_input_requires_prepare(DS_LIBRARY_INPUT_DSLIBRARY_ARCHIVE));
    assert(!ds_library_input_requires_prepare(DS_LIBRARY_INPUT_PRESET_FILE));
    assert(!ds_library_input_requires_prepare(DS_LIBRARY_INPUT_DIRECTORY));

    assert(ds_library_archive_entry_is_safe("Capture GO-TO Bass/Samples/B0.wav"));
    assert(!ds_library_archive_entry_is_safe("../outside.wav"));
    assert(!ds_library_archive_entry_is_safe("/etc/passwd"));
    assert(!ds_library_archive_entry_is_safe("Capture/../../outside.wav"));
    assert(!ds_library_archive_entry_is_safe("__MACOSX/Capture/._preset"));
    assert(!ds_library_archive_entry_is_safe("Capture/"));

    puts("library input tests passed");
    return 0;
}
