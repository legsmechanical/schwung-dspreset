#include "library_import.h"

int ds_library_import_transition_is_valid(ds_library_import_state_t from,
                                          ds_library_import_state_t to) {
    switch (from) {
        case DS_LIBRARY_IMPORT_IDLE:
            return to == DS_LIBRARY_IMPORT_SCANNING;
        case DS_LIBRARY_IMPORT_SCANNING:
            return to == DS_LIBRARY_IMPORT_EXTRACTING || to == DS_LIBRARY_IMPORT_FAILED;
        case DS_LIBRARY_IMPORT_EXTRACTING:
            return to == DS_LIBRARY_IMPORT_VALIDATING || to == DS_LIBRARY_IMPORT_FAILED;
        case DS_LIBRARY_IMPORT_VALIDATING:
            return to == DS_LIBRARY_IMPORT_READY || to == DS_LIBRARY_IMPORT_FAILED;
        case DS_LIBRARY_IMPORT_FAILED:
            return to == DS_LIBRARY_IMPORT_SCANNING;
        case DS_LIBRARY_IMPORT_READY:
        default:
            return 0;
    }
}

int ds_library_import_is_loadable(ds_library_import_state_t state) {
    return state == DS_LIBRARY_IMPORT_READY;
}
