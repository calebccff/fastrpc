#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <assert.h>

#include "fastrpc.h"

#define FASTRPC_IOCTL_INIT_ATTACH_SNS	_IO('R', 8)

#define NAME "adsp_default_listener"

int main(void)
{
   int fd, ret;
   uint32_t handle;

   fd = open("/dev/fastrpc-sdsp", O_RDONLY);
   assert(fd >= 0);

   ret = ioctl(fd, FASTRPC_IOCTL_INIT_ATTACH_SNS);
   assert(!ret);

   ret = listener_create(fd);
   assert(!ret);

   char error[255] = {};
   int err;
   ret = remotectl_open(fd, NAME, sizeof(NAME), &handle, error, sizeof(error), &err);
   assert(!ret && !err);

   ret = ioctl(fd, FASTRPC_IOCTL_INVOKE, &(struct fastrpc_invoke) {
      .handle = handle,
      .sc = 0,
   });
   assert(!ret);

   /* let listener do its job */
   sleep(100);

   return 0;
}
