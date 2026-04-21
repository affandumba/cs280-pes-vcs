// index.c — Staging area implementation // v2
//
// PROVIDED functions: index_find, index_remove, index_status
// IMPLEMENTED functions: index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declaration
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("    staged: %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("    deleted: %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if ((uint64_t)st.st_mtime != index->entries[i].mtime_sec ||
                (uint32_t)st.st_size  != index->entries[i].size) {
                printf("    modified: %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("    untracked: %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("    (nothing to show)\n");
    printf("\n");
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;

    char hex[HASH_HEX_SIZE + 1];
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        unsigned long long mtime_tmp;
        unsigned int size_tmp;
        int result = fscanf(f, "%o %64s %llu %u %511s",
                            &e->mode, hex,
                            &mtime_tmp, &size_tmp,
                            e->path);
        if (result == EOF || result != 5) break;
        e->mtime_sec = (uint64_t)mtime_tmp;
        e->size      = (uint32_t)size_tmp;
        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }
        index->count++;
    }

    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    // Sort using index array to avoid copying huge struct on stack
    int order[MAX_INDEX_ENTRIES];
    for (int i = 0; i < index->count; i++) order[i] = i;

    for (int i = 1; i < index->count; i++) {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 && strcmp(index->entries[order[j]].path,
                                index->entries[key].path) > 0) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < index->count; i++) {
        const IndexEntry *e = &index->entries[order[i]];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                e->mode, hex,
                (unsigned long long)e->mtime_sec,
                (unsigned int)e->size,
                e->path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return rename(tmp_path, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc((size_t)fsize);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    ObjectID oid;
    if (object_write(OBJ_BLOB, buf, (size_t)fsize, &oid) != 0) {
        free(buf); return -1;
    }
    free(buf);

    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    uint32_t mode = S_ISDIR(st.st_mode)    ? 0040000
                  : (st.st_mode & S_IXUSR) ? 0100755
                  : 0100644;

    IndexEntry *existing = index_find(index, path);
    if (existing) {
        existing->hash      = oid;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size      = (uint32_t)st.st_size;
        existing->mode      = mode;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        IndexEntry *e = &index->entries[index->count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->hash      = oid;
        e->mtime_sec = (uint64_t)st.st_mtime;
        e->size      = (uint32_t)st.st_size;
        e->mode      = mode;
    }

    return index_save(index);
}
