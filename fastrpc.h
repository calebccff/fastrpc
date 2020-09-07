#include <stdint.h>
#include <stdbool.h>
#include <misc/fastrpc.h>

int listener_create(int fd);
int user_pd_create(int fd, bool unsigned_pd);
int remotectl_open(int fd, const char *name, uint32_t name_len, uint32_t *handle,
                   char *dlerror, uint32_t dlerror_len, int *err);
