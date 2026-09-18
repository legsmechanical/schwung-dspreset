#ifndef DSPRESET_CATALOG_H
#define DSPRESET_CATALOG_H

/* The banks and presets under a module's instruments/ folder.
 *
 * A BANK is one top-level entry: a folder, a .dslibrary package, or a loose
 * .dspreset. Its PRESETS are the .dspreset files inside it (a .dslibrary's
 * come from its <name>.dslibrary.unpacked/ folder once prepared), in natural
 * order — "2 - Foundation" before "10 - Moog Town".
 *
 * Built on the worker (it reads directories and allocates); published whole
 * and never mutated afterwards, so audio-side readers need no lock. */

enum { DS_BANK_FOLDER = 0, DS_BANK_DSLIBRARY, DS_BANK_FILE };

typedef struct {
    char name[128];                 /* shown in the bank list */
    char path[1024];                /* the folder, .dslibrary or .dspreset */
    int kind;
    int prepared;                   /* a .dslibrary whose .unpacked folder exists */
    unsigned preset_count;
    char (*preset_names)[128];
    char (*preset_paths)[1024];
} ds_bank_t;

typedef struct {
    ds_bank_t *banks;
    unsigned bank_count;
} ds_catalog_t;

/* Returns a new catalog (possibly empty) or NULL on allocation failure. */
ds_catalog_t *ds_catalog_scan(const char *instruments_dir);
void ds_catalog_free(ds_catalog_t *catalog);
/* Same banks, same presets, same order? Lets the worker skip republishing. */
int ds_catalog_equal(const ds_catalog_t *a, const ds_catalog_t *b);
/* Finds a preset by absolute path; returns 0 and fills bank/preset, else -1. */
int ds_catalog_find(const ds_catalog_t *catalog, const char *path, int *bank, int *preset);
int ds_natural_compare(const char *a, const char *b);
/* The natural-order first .dspreset under `dir`; returns 0 when one exists. */
int ds_catalog_first_preset(const char *dir, char *out, unsigned out_len);

#endif
