#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

static off_t min_size    = 1;
static int   scan_hidden = 0;

typedef struct {
    char     *path;
    off_t     size;
    uint64_t  hash;
    dev_t     dev;
    ino_t     ino;
} FileEntry;

typedef struct {
    FileEntry *items;
    size_t     count;
    size_t     capacity;
} FileList;

static void list_add(FileList *list, const char *path, const struct stat *st) {
    if (list->count == list->capacity) {
        size_t new_cap = list->capacity ? list->capacity * 2 : 64;
        FileEntry *tmp = realloc(list->items, new_cap * sizeof *tmp);
        if (!tmp) {
            perror("realloc");
            exit(1);
        }
        list->items = tmp;
        list->capacity = new_cap;
    }
    char *copy = strdup(path);
    if (!copy) {
        perror("strdup");
        exit(1);
    }
    FileEntry *e = &list->items[list->count];
    e->path = copy;
    e->size = st->st_size;
    e->hash = 0;
    e->dev  = st->st_dev;
    e->ino  = st->st_ino;
    list->count++;
}

static void walk(const char *dir, FileList *list) {
    DIR *d = opendir(dir);
    if (!d) {
        perror(dir);
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (!scan_hidden && e->d_name[0] == '.')
            continue;

        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);

        struct stat st;
        if (lstat(path, &st) != 0) {
            perror(path);
            continue;
        }

        if (S_ISDIR(st.st_mode))
            walk(path, list);
        else if (S_ISREG(st.st_mode) && st.st_size >= min_size)
            list_add(list, path, &st);
    }
    closedir(d);
}

static int cmp_size(const void *a, const void *b) {
    const FileEntry *x = a;
    const FileEntry *y = b;
    if (x->size < y->size) return -1;
    if (x->size > y->size) return 1;
    return 0;
}

static int cmp_hash(const void *a, const void *b) {
    const FileEntry *x = a;
    const FileEntry *y = b;
    if (x->hash < y->hash) return -1;
    if (x->hash > y->hash) return 1;
    return 0;
}

static uint64_t fnv1a(const unsigned char *buf, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) {
        h ^= buf[i];
        h *= FNV_PRIME;
    }
    return h;
}

static int hash_file(const char *path, size_t max_bytes, uint64_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }

    unsigned char buf[65536];
    uint64_t h = FNV_OFFSET;
    size_t total = 0;

    for (;;) {
        size_t want = sizeof buf;
        if (max_bytes) {
            if (total >= max_bytes)
                break;
            if (max_bytes - total < want)
                want = max_bytes - total;
        }
        size_t n = fread(buf, 1, want, f);
        if (n == 0)
            break;
        h = fnv1a(buf, n, h);
        total += n;
    }

    int err = ferror(f);
    fclose(f);
    if (err) {
        perror(path);
        return -1;
    }
    *out = h;
    return 0;
}

static size_t filter_by_hash(FileEntry *items, size_t n, size_t max_bytes) {
    size_t ok = 0;
    for (size_t k = 0; k < n; k++) {
        uint64_t h;
        if (hash_file(items[k].path, max_bytes, &h) == 0) {
            items[k].hash = h;
            if (ok != k) {
                FileEntry tmp = items[ok];
                items[ok] = items[k];
                items[k] = tmp;
            }
            ok++;
        }
    }

    qsort(items, ok, sizeof *items, cmp_hash);

    size_t kept = 0;
    size_t a = 0;
    while (a < ok) {
        size_t b = a;
        while (b < ok && items[b].hash == items[a].hash)
            b++;
        if (b - a >= 2) {
            for (size_t c = a; c < b; c++) {
                if (kept != c) {
                    FileEntry tmp = items[kept];
                    items[kept] = items[c];
                    items[c] = tmp;
                }
                kept++;
            }
        }
        a = b;
    }
    return kept;
}

/* Compare two files byte by byte. Returns 1 if identical, 0 if not or on error. */
static int files_identical(const char *p1, const char *p2) {
    FILE *f1 = fopen(p1, "rb");
    FILE *f2 = fopen(p2, "rb");
    int same = 0;

    if (f1 && f2) {
        unsigned char b1[65536], b2[65536];
        same = 1;
        for (;;) {
            size_t n1 = fread(b1, 1, sizeof b1, f1);
            size_t n2 = fread(b2, 1, sizeof b2, f2);
            if (n1 != n2 || memcmp(b1, b2, n1) != 0) {
                same = 0;
                break;
            }
            if (n1 == 0)
                break;
        }
        if (ferror(f1) || ferror(f2))
            same = 0;
    }
    if (f1) fclose(f1);
    if (f2) fclose(f2);
    return same;
}

/* Replace dup with a hardlink to keeper. Returns 0 on success. */
static int link_dup(const char *keeper, const char *dup) {
    char tmp[4096 + 16];
    snprintf(tmp, sizeof tmp, "%s.cdupe.tmp", dup);

    if (link(keeper, tmp) != 0) {
        perror(tmp);
        return -1;
    }
    if (rename(tmp, dup) != 0) {
        perror(dup);
        unlink(tmp);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *root = ".";
    int do_link = 0;

    for (int k = 1; k < argc; k++) {
        if (strcmp(argv[k], "--link") == 0) {
            do_link = 1;
        } else if (strcmp(argv[k], "--hidden") == 0) {
            scan_hidden = 1;
        } else if (strcmp(argv[k], "--min-size") == 0 && k + 1 < argc) {
            min_size = (off_t)atoll(argv[++k]);
        } else if (strcmp(argv[k], "--help") == 0 || strcmp(argv[k], "-h") == 0) {
            printf("cdupe - find duplicate files, optionally replace them with hardlinks\n\n");
            printf("Usage: cdupe [options] [folder]\n\n");
            printf("Options:\n");
            printf("  --link          replace duplicates with hardlinks (default: report only)\n");
            printf("  --min-size N    ignore files smaller than N bytes (default: 1)\n");
            printf("  --hidden        also scan hidden files and folders\n");
            printf("  -h, --help      show this help\n");
            return 0;
        } else {
            root = argv[k];
        }
    }

    FileList list = {0};
    walk(root, &list);

    printf("Scanned %zu files\n", list.count);

    qsort(list.items, list.count, sizeof *list.items, cmp_size);

    size_t groups = 0;
    size_t linked = 0;
    off_t  saved  = 0;

    size_t i = 0;
    while (i < list.count) {
        size_t j = i;
        while (j < list.count && list.items[j].size == list.items[i].size)
            j++;

        if (j - i >= 2) {
            size_t n = filter_by_hash(&list.items[i], j - i, 4096);
            n = filter_by_hash(&list.items[i], n, 0);

            size_t a = 0;
            while (a < n) {
                size_t b = a;
                while (b < n && list.items[i + b].hash == list.items[i + a].hash)
                    b++;

                FileEntry *g = &list.items[i + a];   /* g[0] is the keeper */
                size_t count = b - a;

                /* how many files are real, separate copies of the keeper? */
                size_t extras = 0;
                for (size_t c = 1; c < count; c++)
                    if (!(g[c].dev == g[0].dev && g[c].ino == g[0].ino))
                        extras++;

                if (extras > 0) {
                    groups++;
                    saved += g[0].size * (off_t)extras;
                    printf("\nDuplicate group (%lld bytes each, %zu files):\n",
                           (long long)g[0].size, count);
                    printf("  keep  %s\n", g[0].path);

                    for (size_t c = 1; c < count; c++) {
                        int same_inode = (g[c].dev == g[0].dev && g[c].ino == g[0].ino);
                        if (same_inode) {
                            printf("  skip  %s  (already hardlinked)\n", g[c].path);
                        } else if (!do_link) {
                            printf("  dup   %s\n", g[c].path);
                        } else if (g[c].dev != g[0].dev) {
                            printf("  skip  %s  (different filesystem)\n", g[c].path);
                        } else if (!files_identical(g[0].path, g[c].path)) {
                            printf("  skip  %s  (contents differ, hash collision)\n", g[c].path);
                        } else if (link_dup(g[0].path, g[c].path) == 0) {
                            printf("  link  %s\n", g[c].path);
                            linked++;
                        }
                    }
                }
                a = b;
            }
        }
        i = j;
    }

    printf("\n%zu duplicate group(s), %lld bytes reclaimable\n",
           groups, (long long)saved);
    if (do_link)
        printf("%zu file(s) replaced with hardlinks\n", linked);
    else if (groups > 0)
        printf("Dry run only. Re-run with --link to replace duplicates with hardlinks.\n");

    for (size_t m = 0; m < list.count; m++)
        free(list.items[m].path);
    free(list.items);
    return 0;
}