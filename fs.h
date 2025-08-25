#pragma once

#include "string_ops.h"
#include <linux/limits.h>
#include <sys/stat.h>

#include <stdbool.h>

typedef struct {
    bool exists;
    size_t size;
} fs_metadata;

extern inline fs_metadata fs_get_metadata(string_view filename) {
    char buf[PATH_MAX];
    fs_metadata metadata;
    struct stat st;
    metadata.exists = false;
    if (filename.len + 1 > PATH_MAX) {
        return metadata;
    }
    memcpy(buf, filename.data, filename.len);
    buf[filename.len] = 0;

    if (stat(buf, &st) < 0) {
        return metadata;
    }

    metadata.size = st.st_size;
    metadata.exists = true;
    return metadata;
}
