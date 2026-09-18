#include <assert.h>
#include <stdio.h>

#include "../src/dsp/dspreset/library_import.h"

int main(void) {
    assert(ds_library_import_transition_is_valid(DS_LIBRARY_IMPORT_IDLE,
                                                  DS_LIBRARY_IMPORT_SCANNING));
    assert(ds_library_import_transition_is_valid(DS_LIBRARY_IMPORT_SCANNING,
                                                  DS_LIBRARY_IMPORT_EXTRACTING));
    assert(ds_library_import_transition_is_valid(DS_LIBRARY_IMPORT_EXTRACTING,
                                                  DS_LIBRARY_IMPORT_VALIDATING));
    assert(ds_library_import_transition_is_valid(DS_LIBRARY_IMPORT_VALIDATING,
                                                  DS_LIBRARY_IMPORT_READY));
    assert(!ds_library_import_is_loadable(DS_LIBRARY_IMPORT_EXTRACTING));
    assert(ds_library_import_is_loadable(DS_LIBRARY_IMPORT_READY));
    assert(!ds_library_import_transition_is_valid(DS_LIBRARY_IMPORT_READY,
                                                   DS_LIBRARY_IMPORT_SCANNING));
    assert(ds_library_import_transition_is_valid(DS_LIBRARY_IMPORT_FAILED,
                                                  DS_LIBRARY_IMPORT_SCANNING));
    puts("library import tests passed");
    return 0;
}
