/* dirent.h — directory streams (NovaOS) */
#pragma once
#include <sys/types.h>
_NOVA_BEGIN
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[260];
};
typedef struct __nova_dir DIR;
_CRTIMP DIR           *opendir(const char *path);
_CRTIMP struct dirent *readdir(DIR *d);
_CRTIMP int            closedir(DIR *d);
_CRTIMP void           rewinddir(DIR *d);
_CRTIMP int scandir(const char *dir, struct dirent ***list,
                    int (*filter)(const struct dirent *),
                    int (*cmp)(const struct dirent **, const struct dirent **));
_CRTIMP int alphasort(const struct dirent **a, const struct dirent **b);
_NOVA_END
