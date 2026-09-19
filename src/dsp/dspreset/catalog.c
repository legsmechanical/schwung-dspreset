#include "catalog.h"
#include "dspreset_parser.h"

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

/* ---- DSLibraryInfo.xml -------------------------------------------------
 * A library's own name and preset menu. The file sits at the library's top
 * level — for a .dslibrary, usually inside the one folder the archive holds.
 * <presetMenu> regroups presets without moving them: top-level entries (its
 * menus and every preset it does not mention) are alphabetical, a menu keeps
 * the order it was written in, nested menus read "Pads / Analog / Drift" in
 * the Move's flat list, missing files are skipped and empty menus dropped. */

#define MAX_MENU_DEPTH 8

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    char *text = NULL;
    if (!f) return NULL;
    if (!fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && n < (1 << 20) && !fseek(f, 0, SEEK_SET) &&
        (text = malloc((size_t)n + 1)) && fread(text, 1, (size_t)n, f) == (size_t)n) text[n] = '\0';
    else { free(text); text = NULL; }
    fclose(f);
    return text;
}

/* The folder DSLibraryInfo.xml lives in (root, or root's only folder); 0 if found. */
static int library_base(const char *root, char *base, size_t n) {
    char path[1200], only[1024] = "";
    struct stat st;
    DIR *dir;
    struct dirent *entry;
    int folders = 0;
    /* (a path too long for these buffers is no library info) */
    if (snprintf(path, sizeof(path), "%s/DSLibraryInfo.xml", root) >= (int)sizeof(path)) return -1;
    if (!stat(path, &st)) return snprintf(base, n, "%s", root) >= (int)n ? -1 : 0;
    if (!(dir = opendir(root))) return -1;
    while ((entry = readdir(dir)) != NULL) {
        char sub[1100];
        if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "__MACOSX")) continue;
        if (snprintf(sub, sizeof(sub), "%s/%s", root, entry->d_name) >= (int)sizeof(sub)) continue;
        if (!stat(sub, &st) && S_ISDIR(st.st_mode) && snprintf(only, sizeof(only), "%s", sub) < (int)sizeof(only)) folders++;
    }
    closedir(dir);
    if (folders != 1) return -1;
    if (snprintf(path, sizeof(path), "%s/DSLibraryInfo.xml", only) >= (int)sizeof(path) || stat(path, &st)) return -1;
    return snprintf(base, n, "%s", only) >= (int)n ? -1 : 0;
}

typedef struct { int found, top; char label[128]; } menu_entry_t;

static int by_top_name(const void *a, const void *b) {
    return ds_natural_compare(((const found_t *)a)->name, ((const found_t *)b)->name);
}

static void apply_library_info(ds_bank_t *bank, const char *root, found_list_t *list) {
    char base[1024], path[1200], *xml, *p;
    char stack[MAX_MENU_DEPTH][128];
    int depth = 0, in_menu = 0, top = -1, tops = 0;
    menu_entry_t *entries;
    found_t *order, *out;
    unsigned char *used;
    unsigned n = 0, count = 0;
    if (library_base(root, base, sizeof(base))) return;
    if (snprintf(path, sizeof(path), "%s/DSLibraryInfo.xml", base) >= (int)sizeof(path) || !(xml = read_text(path))) return;
    entries = malloc(MAX_PRESETS * sizeof(*entries));
    order = malloc(2 * MAX_PRESETS * sizeof(*order));             /* top-level keys, then the result */
    used = calloc(list->count ? list->count : 1, 1);
    if (!entries || !order || !used) goto done;
    out = order + MAX_PRESETS;
    for (p = xml; (p = strchr(p, '<')) != NULL; ) {
        char *tag = p + 1, *end = strchr(tag, '>'), text[512];
        int closing = 0, self_closing;
        if (!end) break;
        p = end + 1;
        if (*tag == '/') { closing = 1; ++tag; }
        self_closing = end > tag && end[-1] == '/';
        if (!strncmp(tag, "DecentSamplerLibraryInfo", 24) && !closing) {
            if (ds_xml_attribute(tag, end, "name", text, sizeof(text)) && text[0]) snprintf(bank->name, sizeof(bank->name), "%.127s", text);
        } else if (!strncmp(tag, "presetMenu", 10)) {
            in_menu = !closing && !self_closing;
        } else if (in_menu && !strncmp(tag, "menu", 4) && (tag[4] == ' ' || tag[4] == '>' || tag[4] == '/' || tag[4] == '\t' || tag[4] == '\n' || tag[4] == '\r')) {
            if (closing) { if (depth) depth--; continue; }
            if (self_closing) continue;                           /* a menu with nothing in it */
            if (depth == 0) top = tops++;
            if (depth < MAX_MENU_DEPTH) {
                if (!ds_xml_attribute(tag, end, "name", stack[depth], sizeof(stack[0]))) snprintf(stack[depth], sizeof(stack[0]), "Menu");
            }
            depth++;
        } else if (in_menu && !closing && !strncmp(tag, "preset", 6) && !isalnum((unsigned char)tag[6]) &&
                   ds_xml_attribute(tag, end, "file", text, sizeof(text)) && n < MAX_PRESETS) {
            char full[1600], *q = text;
            int hit = -1;
            for (char *c = text; *c; ++c) if (*c == '\\') *c = '/';
            while (q[0] == '.' && q[1] == '/') q += 2;
            if (snprintf(full, sizeof(full), "%s/%s", base, q) >= (int)sizeof(full)) continue;
            for (unsigned i = 0; i < list->count && hit < 0; ++i) if (!strcmp(list->items[i].path, full)) hit = (int)i;
            for (unsigned i = 0; i < list->count && hit < 0; ++i) if (!strcasecmp(list->items[i].path, full)) hit = (int)i;
            if (hit < 0) continue;                                /* a missing file is skipped */
            used[hit] = 1;
            entries[n].found = hit;
            entries[n].top = depth ? top : -1;
            entries[n].label[0] = '\0';
            for (int d = 0; d < depth && d < MAX_MENU_DEPTH; ++d) {
                size_t len = strlen(entries[n].label);
                snprintf(entries[n].label + len, sizeof(entries[n].label) - len, "%s / ", stack[d]);
            }
            {
                size_t len = strlen(entries[n].label);
                snprintf(entries[n].label + len, sizeof(entries[n].label) - len, "%s", list->items[hit].name);
            }
            n++;
        }
    }
    if (!n) goto done;                                            /* nothing usable: the plain list */
    /* top level: each non-empty menu (by its name), each preset outside any menu */
    for (int t = 0; t < tops; ++t) {
        int first = -1;
        for (unsigned k = 0; k < n && first < 0; ++k) if (entries[k].top == t) first = (int)k;
        if (first < 0) continue;                                  /* an empty menu is dropped */
        snprintf(order[count].name, sizeof(order[0].name), "%.127s", entries[first].label);
        { char *cut = strstr(order[count].name, " / "); if (cut) *cut = '\0'; }
        snprintf(order[count].path, sizeof(order[0].path), "m%d", t);
        count++;
    }
    for (unsigned k = 0; k < n; ++k)                              /* presets <presetMenu> lists outside a menu */
        if (entries[k].top < 0) {
            snprintf(order[count].name, sizeof(order[0].name), "%s", entries[k].label);
            snprintf(order[count].path, sizeof(order[0].path), "e%u", k);
            count++;
        }
    for (unsigned i = 0; i < list->count && count < MAX_PRESETS; ++i)   /* and every preset it does not mention */
        if (!used[i]) {
            order[count] = list->items[i];
            snprintf(order[count].path, sizeof(order[0].path), "u%u", i);
            count++;
        }
    qsort(order, count, sizeof(found_t), by_top_name);
    {
        unsigned made = 0;
        for (unsigned t = 0; t < count && made < MAX_PRESETS; ++t) {
            char kind = order[t].path[0];
            long index = strtol(order[t].path + 1, NULL, 10);
            if (kind == 'u') { out[made++] = list->items[index]; continue; }
            if (kind == 'e') {
                out[made] = list->items[entries[index].found];
                snprintf(out[made].name, sizeof(out[0].name), "%s", entries[index].label);
                made++;
                continue;
            }
            for (unsigned k = 0; k < n && made < MAX_PRESETS; ++k)
                if (entries[k].top == index) {
                    out[made] = list->items[entries[k].found];
                    snprintf(out[made].name, sizeof(out[0].name), "%s", entries[k].label);
                    made++;
                }
        }
        memcpy(list->items, out, made * sizeof(found_t));
        list->count = made;
    }
done:
    free(xml); free(entries); free(order); free(used);
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
    if (bank->kind == DS_BANK_FOLDER) apply_library_info(bank, bank->path, &list);
    else if (bank->kind == DS_BANK_DSLIBRARY && bank->prepared) apply_library_info(bank, unpacked, &list);
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
            if (S_ISDIR(st.st_mode)) { b->kind = DS_BANK_FOLDER; strip(b->name, ".dsbundle"); }   /* a macOS bundle is a folder */
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
        if (strcmp(x->path, y->path) || strcmp(x->name, y->name) || x->prepared != y->prepared ||
            x->preset_count != y->preset_count) return 0;
        for (unsigned p = 0; p < x->preset_count; ++p)
            if (strcmp(x->preset_paths[p], y->preset_paths[p]) || strcmp(x->preset_names[p], y->preset_names[p])) return 0;
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
