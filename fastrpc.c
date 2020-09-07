#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#include <assert.h>
#include <dirent.h>

#include "fastrpc.h"

#define FASTRPC_STATIC_HANDLE_REMOTECTL 0
#define FASTRPC_STATIC_HANDLE_LISTENER 3

#define ALIGN_POT(x, y) (((x) + (y) - 1) & ~((y) - 1))

#define invoke_arg(x) (struct fastrpc_invoke_args) { (size_t) x, sizeof(x) }

struct bytearray {
   uint32_t *ptr;
   uint32_t len;
};

static void* listener_thread(void *arg)
{
   int fd = (size_t) arg, ret;
   uint32_t in_buf[4096] __attribute__((aligned(4096))) = {};
   uint32_t out_buf[4096] __attribute__((aligned(4096))) = {};
   uint32_t msg_send[4];
   uint32_t msg_recv[4];
   uint32_t ctx = 0;
   uint32_t handle = -1;
   int r = -1;
   struct bytearray args[256], out_args[256];
   unsigned out_bufs_len = 0;

   do {
      /* adsp_listener_next2() */

      msg_send[0] = ctx;
      msg_send[1] = r;
      msg_send[2] = out_bufs_len; /* size_in */
      msg_send[3] = sizeof(in_buf);

      ret = ioctl(fd, FASTRPC_IOCTL_INVOKE, &(struct fastrpc_invoke) {
         .handle = FASTRPC_STATIC_HANDLE_LISTENER,
         .sc = 0x04020200,
         .args = (size_t) (struct fastrpc_invoke_args[4]) {
            invoke_arg(msg_send),
            { (size_t) out_buf, out_bufs_len },
            invoke_arg(msg_recv),
            invoke_arg(in_buf),
         },
      });
      assert(!ret);
      ctx = msg_recv[0];
      handle = msg_recv[1];

      assert(msg_recv[3] <= sizeof(in_buf));

      uint32_t sc = msg_recv[2];

      // printf("handle invoke: sc=%.8x %.8x\n", sc, handle);

      unsigned inbufs = sc >> 16 & 0xff;
      unsigned outbufs = sc >> 8 & 0xff;

      void *ptr = in_buf;
      for (unsigned i = 0; i < inbufs; i++) {
         memcpy(&args[i].len, ptr, 4); ptr += 4;
         ptr = (void*) (((size_t) ptr + 7) & ~7); /* align 8 */
         args[i].ptr = ptr; ptr += args[i].len;

         /* note: real lengths come from arg[0] dword array */
         // printf("  inbuf %d: %d %.*s\n", i, args[i].len, args[i].len, args[i].ptr);
      }

      void *ptr_out = out_buf;
      for (unsigned i = 0; i < outbufs; i++) {
         memcpy(&out_args[i].len, ptr, 4); ptr += 4;

         memcpy(ptr_out, &out_args[i].len, 4); ptr_out += 4;
         ptr_out = (void*) (((size_t) ptr_out + 7) & ~7); /* align 8 */
         out_args[i].ptr = ptr_out; ptr_out += out_args[i].len;

         // printf("  outbuf %d: %d\n", i, out_args[i].len);
      }
      out_bufs_len = ptr_out - (void*) out_buf;

#define APPS_FD_BASE 0

      switch (sc) {
      case 0x00020200: /* fopen */
         // XXX
         out_args[0].ptr[0] = 0x18cb1088;
         memset(out_args[1].ptr, 0, 0xff);
         r = 0;
         break;
      case 0x01010200: /* freopen */
         // XXX
         out_args[0].ptr[0] = 0x18cb1088;
         memset(out_args[1].ptr, 0, 0xff);
         r = 0;
         break;
      case 0x02010100: { /* map64 (XXX apps_mem) */
         uint32_t heapid = args[0].ptr[0];
         uint32_t lflags = args[0].ptr[1];
         uint32_t rflags = args[0].ptr[2];
         uint64_t vin = *(uint64_t*) &args[0].ptr[4];
         uint64_t len = *(uint64_t*) &args[0].ptr[6];
         //printf("map64 %.8x %.8x %.8x %.16lx %.16lx\n",
         //       heapid, lflags, rflags, vin, len);
#define ADSP_MMAP_ADD_PAGES   0x1000
         assert(rflags == ADSP_MMAP_ADD_PAGES);

         // allocate a fd with vaddrin??
         struct fastrpc_req_mmap mmap = {
            .fd = -1,
            .flags = rflags,
            .vaddrin = 0,
            .size = len,
         };

         r = ioctl(fd, FASTRPC_IOCTL_MMAP, &mmap);
         assert(r == 0);

         *(uint64_t*) &out_args[0].ptr[0] = 0; /* vapps */
         *(uint64_t*) &out_args[0].ptr[2] = mmap.vaddrout; /* vadsp */
         r = 0;
      } break;
      case 0x13050100: { /* fopen_with_env */
         /* args {lengths, envvarname, delim, name, mode} */
#ifndef PREFIX
         const char *path = (char*) args[3].ptr;
#else
         char path[256];
         sprintf(path, PREFIX "%s", args[3].ptr);
#endif
         r = open(path, O_RDONLY); /* OK to assume null terminated string? */
         if (r >= 0) {
            out_args[0].ptr[0] = APPS_FD_BASE + r;
            r = 0;
         } else {
            r = 2;
         }
      } break;
      case 0x09010000: /* fseek [fd, offset, whence] */
         r = lseek(args[0].ptr[0] - APPS_FD_BASE, args[0].ptr[1], args[0].ptr[2]);
         r = r >= 0 ? 0 : r;
         break;
      case 0x03010000: /* fclose */
         close(args[0].ptr[0] - APPS_FD_BASE);
         r = 0;
         break;
      case 0x04010200: /* fread in: [fd, length] out: [bytesread, eof] [data] */
         r = read(args[0].ptr[0] - APPS_FD_BASE, out_args[1].ptr, args[0].ptr[1]);
         if (r >= 0) {
            out_args[0].ptr[0] = r;
            out_args[0].ptr[1] = r == 0;
            r = 0;
         }
         break;
      case 0x1a020100: { /* opendir */
#ifndef PREFIX
         const char *path = (char*) args[1].ptr;
#else
         char path[256];
         sprintf(path, PREFIX "%s", args[1].ptr);
#endif
         DIR *dir = opendir(path);
         *(uint64_t*) out_args[0].ptr = (size_t) dir;
         r = dir ? 0 : 2;
      } break;
      case 0x1b010000: /* closedir */
         closedir((DIR*) *(uint64_t*) args[0].ptr);
         break;
      case 0x1c010100: { /* readdir */
         struct apps_std_DIRENT {
            int ino;
            char name[256];
            uint32_t eof;
         } *dirent = (void*) out_args[0].ptr;

         struct dirent *odirent = readdir((DIR*) *(uint64_t*) args[0].ptr);

         if (odirent) {
            dirent->ino = (int)odirent->d_ino;
            strcpy(dirent->name, odirent->d_name);
         } else {
            dirent->eof = 1;
         }
      } break;
      case 0x1f020100: { /* stat. note: shares mid with ftrunc/frename */
         struct stat st;
         int f;
#ifndef PREFIX
         const char *path = (char*) args[1].ptr;
#else
         char path[256];
         sprintf(path, PREFIX "%s", args[1].ptr);
#endif
         f = open(path, O_RDONLY); /* OK to assume null terminated string? */
         assert(f >= 0);

         r = fstat(f, &st);
         assert(!r);

         close(f);

         struct apps_std_STAT {
            uint64_t tsz;
            uint64_t dev;
            uint64_t ino;
            uint32_t mode;
            uint32_t nlink;
            uint64_t rdev;
            uint64_t size;
            int64_t atime;
            int64_t atimensec;
            int64_t mtime;
            int64_t mtimensec;
            int64_t ctime;
            int64_t ctimensec;
         } *ist = (void*) out_args[0].ptr;

         ist->dev = st.st_dev;
         ist->ino = st.st_ino;
         ist->mode = st.st_mode;
         ist->nlink = st.st_nlink;
         ist->rdev = st.st_rdev;
         ist->size = st.st_size;
         ist->atime = (int64_t)st.st_atim.tv_sec;
         ist->atimensec = (int64_t)st.st_atim.tv_nsec;
         ist->mtime = (int64_t)st.st_mtim.tv_sec;
         ist->mtimensec =(int64_t)st.st_mtim.tv_nsec;
         ist->ctime = (int64_t)st.st_ctim.tv_nsec;
         ist->ctimensec = (int64_t)st.st_ctim.tv_nsec;
      } break;
      default:
         printf("unexpected sc %.8x\n", sc);
         assert(0);
         break;
      }
   } while (1);
}

int listener_create(int fd)
{
   /* adsp_listener_init2() */
   int ret = ioctl(fd, FASTRPC_IOCTL_INVOKE, &(struct fastrpc_invoke) {
      .handle = FASTRPC_STATIC_HANDLE_LISTENER,
      .sc = 0x03000000,
   });
   assert(!ret);

   pthread_t thread;
   pthread_create(&thread, NULL, listener_thread, (void*) (size_t) fd);

   return 0;
}

int user_pd_create(int rpc_fd, bool unsigned_pd)
{
   struct stat stat;
   int fd, ret;
   void *file_ptr;

   if (unsigned_pd)
      fd = open("fastrpc_shell_unsigned_3", O_RDONLY);
   else
      fd = open("fastrpc_shell_3", O_RDONLY);

   assert(fd >= 0);

   ret = fstat(fd, &stat);
   assert(!ret);

   struct fastrpc_alloc_dma_buf buf = {
      .fd = -1,
      .flags = 0,
      .size = ALIGN_POT(stat.st_size, 4096), /* XXX: fastrpc_alloc rounds down otherwise? */
   };
   ret = ioctl(rpc_fd, FASTRPC_IOCTL_ALLOC_DMA_BUFF, &buf);
   assert(!ret);

   file_ptr = mmap(0, stat.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, buf.fd, 0);
   assert(file_ptr != MAP_FAILED);

   ret = read(fd, file_ptr, stat.st_size);
   assert(ret == stat.st_size);

   ret = ioctl(rpc_fd, FASTRPC_IOCTL_INIT_CREATE, &(struct fastrpc_init_create) {
      .filelen = stat.st_size,
      .filefd = buf.fd,
      .attrs = unsigned_pd ? 8 : 0, /* TODO: is this right? */
      .siglen = 0,
      .file = (size_t) file_ptr,
   });

  munmap(file_ptr, stat.st_size);
  /* TODO: save buf.fd somewhere or free it? */

  return ret;
}

int remotectl_open(int fd, const char *name, uint32_t name_len, uint32_t *handle,
                   char *dlerror, uint32_t dlerror_len, int *err)
{
   uint32_t out[2];
   int ret = ioctl(fd, FASTRPC_IOCTL_INVOKE, &(struct fastrpc_invoke) {
      .handle = FASTRPC_STATIC_HANDLE_REMOTECTL,
      .sc = 0x00020200,
      .args = (size_t) (struct fastrpc_invoke_args[4]) {
         { (size_t) (uint32_t[2]) { name_len, dlerror_len }, 8},
         { (size_t) name, name_len },
         { (size_t) out, sizeof(out) },
         { (size_t) dlerror, dlerror_len }
      },
   });
   if (ret)
      return ret;

   *handle = out[0];
   *err = out[1];
   return 0;
}
