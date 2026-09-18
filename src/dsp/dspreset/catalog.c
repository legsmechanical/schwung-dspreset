#include "catalog.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_BANKS 256
#define MAX_PRESETS 1024
#define MAX_DEPTH 5

static int has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && !strcasecmp(s + n - m, suffix);
}

int ds_natural_compare(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea, *eb;
            unsigned long x = strtoul(a, &ea, 10), y = strtoul(b, &eb, 10);
            if (x != y) return x < y ? -1 : 1;
            a = ea; b = eb;
            continue;
        }
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return tolower((unsigned char)*a) - tolower((unsigned char)*b);
        ++a; ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void strip(char *name, const char *suffix) {
    size_t n = strlen(name), m = strlen(suffix);
    if (n > m && !strcasecmp(name + n - m, suffix)) name[n - m] = '\0';
}

/* ---- presets inside one bank ------------------------------------------- */

typedef struct { char path[1024]; char name[128]; } found_t;
typedef struct { found_t *items; unsigned count; } found_list_t;

static int by_path(const void *a, const void *b) {
    return ds_natural_compare(((const found_t *)a)->path, ((const found_t *)b)->path);
}

static void collect_presets(const char *dir_path, found_list_t *out, int depth) {
    DIR *dir;
    struct dirent *entry;
    if (depth > MAX_DEPTH || !(dir = opendir(dir_path))) return;
    while ((entry = readdir(dir)) != NULL && out->count < MAX_PRESETS) {
        char path[1024];
        struct stat st;
        if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "__MACOSX")) continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name) >= (int)sizeof(path) ||
            stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) collect_presets(path, out, depth + 1);
        else if (S_ISREG(st.st_mode) && has_suffix(entry->d_name, ".dspreset")) {
            found_t *f = &out->items[out->count++];
            snprintf(f->path, sizeof(f->path), "%s", path);
            snprintf(f->name, sizeof(f->name), "%.127s", entry->d_name);   /* display only */
            strip(f->name, ".dspreset");
        }
    }
    closedir(dir);
}

static int fill_presets(ds_bank_t *bank, found_t *scratch) {
    found_list_t list = {scratch, 0};
    char unpacked[1100];
    if (bank->kind == DS_BANK_FILE) {
        snprintf(scratch[0].path, sizeof(scratch[0].path), "%s", bank->path);
        snprintf(scratch[0].name, sizeof(scratch[0].name), "%s", bank->name);
        list.count = 1;
    } else if (bank->kind == DS_BANK_DSLIBRARY) {
        struct stat st;
        snprintf(unpacked, sizeof(unpacked), "%s.unpacked", bank->path);
        bank->prepared = !stat(unpacked, &st) && S_ISDIR(st.st_mode);
        if (bank->prepared) collect_presets(unpacked, &list, 0);
    } else {
        collect_presets(bank->path, &list, 0);
    }
    qsort(list.items, list.count, sizeof(found_t), by_path);
    bank->preset_count = list.count;
    if (!list.count) return 0;
    bank->preset_names = malloc(list.count * sizeof(bank->preset_names[0]));
    bank->preset_paths = malloc(list.count * sizeof(bank->preset_paths[0]));
    if (!bank->preset_names || !bank->preset_paths) return -1;
    for (unsigned i = 0; i < list.count; ++i) {
        memcpy(bank->preset_names[i], list.items[i].name, sizeof(bank->preset_names[0]));
        memcpy(bank->preset_paths[i], list.items[i].path, sizeof(bank->preset_paths[0]));
    }
    return 0;
}

int ds_catalog_first_preset(const char *dir, char *out, unsigned out_len) {
    found_t *scratch = malloc(MAX_PRESETS * sizeof(found_t));
    found_list_t list;
    int rc = -1;
    if (!scratch) return -1;
    list = (found_list_t){scratch, 0};
    collect_presets(dir, &list, 0);
    if (list.count) {
        qsort(list.items, list.count, sizeof(found_t), by_path);
        snprintf(out, out_len, "%s", list.items[0].path);
        rc = 0;
    }
    free(scratch);
    return rc;
}

/* ---- banks -------------------------------------------------------------- */

static int by_name(const void *a, const void *b) {
    return ds_natural_compare(((const ds_bank_t *)a)->name, ((const ds_bank_t *)b)->name);
}

ds_catalog_t *ds_catalog_scan(const char *instruments_dir) {
    ds_catalog_t *c = calloc(1, sizeof(*c));
    found_t *scratch = malloc(MAX_PRESETS * sizeof(found_t));
    DIR *dir;
    struct dirent *entry;
    unsigned kept = 0;
    if (!c || !scratch) { free(c); free(scratch); return NULL; }
    c->banks = calloc(MAX_BANKS, sizeof(ds_bank_t));
    if (!c->banks) { free(c); free(scratch); return NULL; }
    if ((dir = opendir(instruments_dir)) != NULL) {
        while ((entry = readdir(dir)) != NULL && c->bank_count < MAX_BANKS) {
            ds_bank_t *b = &c->banks[c->bank_count];
            struct stat st;
            if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "__MACOSX") ||
                has_suffix(entry->d_name, ".dslibrary.unpacked") ||
                has_suffix(entry->d_name, ".dslibrary.unpacked.importing")) continue;
            if (snprintf(b->path, sizeof(b->path), "%s/%s", instruments_dir, entry->d_name) >= (int)sizeof(b->path) ||
                stat(b->path, &st)) continue;
            snprintf(b->name, sizeof(b->name), "%.127s", entry->d_name);   /* display only */
            if (S_ISDIR(st.st_mode)) b->kind = DS_BANK_FOLDER;
            else if (S_ISREG(st.st_mode) && has_suffix(entry->d_name, ".dslibrary")) { b->kind = DS_BANK_DSLIBRARY; strip(b->name, ".dslibrary"); }
            else if (S_ISREG(st.st_mode) && has_suffix(entry->d_name, ".dspreset")) { b->kind = DS_BANK_FILE; strip(b->name, ".dspreset"); }
            else continue;
            c->bank_count++;
        }
        closedir(dir);
    }
    /* Folders with no presets are not banks; an unprepared .dslibrary still is. */
    for (unsigned i = 0; i < c->bank_count; ++i) {
        ds_bank_t *b = &c->banks[i];
        if (fill_presets(b, scratch)) { free(scratch); ds_catalog_free(c); return NULL; }
        if (b->preset_count || b->kind == DS_BANK_DSLIBRARY) c->banks[kept++] = *b;
        else { free(b->preset_names); free(b->preset_paths); }
    }
    c->bank_count = kept;
    qsort(c->banks, c->bank_count, sizeof(ds_bank_t), by_name);
    free(scratch);
    return c;
}

void ds_catalog_free(ds_catalog_t *c) {
    if (!c) return;
    for (unsigned i = 0; i < c->bank_count; ++i) { free(c->banks[i].preset_names); free(c->banks[i].preset_paths); }
    free(c->banks);
    free(c);
}

int ds_catalog_equal(const ds_catalog_t *a, const ds_catalog_t *b) {
    if (!a || !b || a->bank_count != b->bank_count) return 0;
    for (unsigned i = 0; i < a->bank_count; ++i) {
        const ds_bank_t *x = &a->banks[i], *y = &b->banks[i];
        if (strcmp(x->path, y->path) || x->prepared != y->prepared || x->preset_count != y->preset_count) return 0;
        for (unsigned p = 0; p < x->preset_count; ++p)
            if (strcmp(x->preset_paths[p], y->preset_paths[p])) return 0;
    }
    return 1;
}

int ds_catalog_find(const ds_catalog_t *c, const char *path, int *bank, int *preset) {
    if (!c || !path) return -1;
    for (unsigned i = 0; i < c->bank_count; ++i) {
        const ds_bank_t *b = &c->banks[i];
        if (b->kind == DS_BANK_DSLIBRARY && !strcmp(b->path, path)) { *bank = (int)i; *preset = 0; return 0; }
        for (unsigned p = 0; p < b->preset_count; ++p)
            if (!strcmp(b->preset_paths[p], path)) { *bank = (int)i; *preset = (int)p; return 0; }
    }
    return -1;
}
