#ifndef DSPRESET_LIBRARY_IMPORT_H
#define DSPRESET_LIBRARY_IMPORT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS_LIBRARY_IMPORT_IDLE = 0,
    DS_LIBRARY_IMPORT_SCANNING,
    DS_LIBRARY_IMPORT_EXTRACTING,
    DS_LIBRARY_IMPORT_VALIDATING,
    DS_LIBRARY_IMPORT_READY,
    DS_LIBRARY_IMPORT_FAILED,
} ds_library_import_state_t;

/* A library is visible to the preset browser only after validation and an
 * atomic directory rename have completed. The import worker alone advances
 * this state; render_block only consumes READY snapshots. */
int ds_library_import_transition_is_valid(ds_library_import_state_t from,
                                          ds_library_import_state_t to);
int ds_library_import_is_loadable(ds_library_import_state_t state);

#ifdef __cplusplus
}
#endif

#endif
