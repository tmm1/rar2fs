/*
    Copyright (C) 2009-2013 Hans Beckerus (hans.beckerus@gmail.com)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    This program take use of the freeware "Unrar C++ Library" (libunrar)
    by Alexander Roshal and some extensions to it.

    Unrar source may be used in any software to handle RAR archives
    without limitations free of charge, but cannot be used to re-create
    the RAR compression algorithm, which is proprietary. Distribution
    of modified Unrar source in separate form or as a part of other
    software is permitted, provided that it is clearly stated in
    the documentation and source comments that the code may not be used
    to develop a RAR (WinRAR) compatible archiver.
*/
#include <platform.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <fuse.h>
#include <fcntl.h>
#include <getopt.h>
#include <syslog.h>
#ifdef HAVE_WCHAR_H
# include <wchar.h>
#endif
#ifdef HAVE_LOCALE_H
# include <locale.h>
#endif
#ifdef HAVE_MMAP
# include <sys/mman.h>
#endif
#include <limits.h>
#include <pthread.h>
#include <ctype.h>
#ifdef HAVE_SCHED_H
# include <sched.h>
#endif
#ifdef HAVE_SYS_XATTR_H
# include <sys/xattr.h>
#endif
#include <assert.h>
#include "version.h"
#include "debug.h"
#include "index.h"
#include "dllwrapper.h"
#include "filecache.h"
#include "iobuffer.h"
#include "optdb.h"
#include "sighandler.h"
#include "dirlist.h"

#define E_TO_MEM 0
#define E_TO_TMP 1

#define MOUNT_FOLDER  0
#define MOUNT_ARCHIVE 1

struct vol_handle {
        FILE *fp;
        off_t pos;
};

struct io_context {
        FILE* fp;
        off_t pos;
        struct io_buf *buf;
        pid_t pid;
        unsigned int seq;
        short vno;
        dir_elem_t *entry_p;
        struct vol_handle *volHdl;
        int pfd1[2];
        int pfd2[2];
        volatile int terminate;
        pthread_t thread;
        pthread_mutex_t mutex;
        /* mmap() data */
        void *mmap_addr;
        FILE *mmap_fp;
        int mmap_fd;
        /* debug */
#ifdef DEBUG_READ
        FILE *dbg_fp;
#endif
};

struct io_handle {
        int type;
#define IO_TYPE_NRM 0
#define IO_TYPE_RAR 1
#define IO_TYPE_RAW 2
#define IO_TYPE_ISO 3
        union {
                struct io_context *context;     /* type = IO_TYPE_RAR/IO_TYPE_RAW */
                int fd;                         /* type = IO_TYPE_NRM/IO_TYPE_ISO */
                uintptr_t bits;
        } u;
        dir_elem_t *entry_p;                    /* type = IO_TYPE_ISO */
};

#define FH_ZERO(fh)            ((fh) = 0)
#define FH_ISSET(fh)           (fh)
#define FH_SETCONTEXT(fh, v)   (FH_TOIO(fh)->u.context = (v))
#define FH_SETFD(fh, v)        (FH_TOIO(fh)->u.fd = (v))
#define FH_SETIO(fh, v)        ((fh) = (uintptr_t)(v))
#define FH_SETENTRY(fh, v)     (FH_TOIO(fh)->entry_p = (v))
#define FH_SETTYPE(fh, v)      (FH_TOIO(fh)->type = (v))
#define FH_TOCONTEXT(fh)       (FH_TOIO(fh)->u.context)
#define FH_TOFD(fh)            (FH_TOIO(fh)->u.fd)
#define FH_TOENTRY(fh)         (FH_TOIO(fh)->entry_p)
#define FH_TOIO(fh)            ((struct io_handle*)(uintptr_t)(fh))

#define WAIT_THREAD(pfd) \
        do {\
                int fd = pfd[0];\
                int nfsd = fd+1;\
                fd_set rd;\
                FD_ZERO(&rd);\
                FD_SET(fd, &rd);\
                int retval = select(nfsd, &rd, NULL, NULL, NULL); \
                if (retval == -1) {\
                        if (errno != EINTR) \
                                perror("select()");\
                } else if (retval) {\
                        /* FD_ISSET(0, &rfds) will be true. */\
                        char buf[2];\
                        NO_UNUSED_RESULT read(fd, buf, 1); /* consume byte */\
                        printd(4, "%lu thread wakeup (%d, %u)\n",\
                               (unsigned long)pthread_self(),\
                               retval,\
                               (int)buf[0]);\
                } else {\
                        perror("select()");\
                }\
        } while (0)

#define WAKE_THREAD(pfd, op) \
        do {\
                /* Wakeup the reader thread */ \
                char buf[2]; \
                buf[0] = (op); \
                if (write((pfd)[1], buf, 1) != 1) \
                        perror("write"); \
        } while (0)

/* Obsolete function(s) */
/*#define ENABLE_OBSOLETE_ARGS*/
#define USE_RAR_PASSWORD

long page_size = 0;
static int mount_type;
struct dir_entry_list arch_list_root;        /* internal list root */
struct dir_entry_list *arch_list = &arch_list_root;
pthread_attr_t thread_attr;
unsigned int rar2_ticks;
int fs_terminated = 0;
int fs_loop = 0;
mode_t umask_ = 0022;

static int extract_rar(char *arch, const char *file, char *passwd, FILE *fp,
                        void *arg);

struct eof_data {
        off_t toff;
        off_t coff;
        size_t size;
        int fd;
};
static int extract_index(const dir_elem_t *entry_p, off_t offset);
static int preload_index(struct io_buf *buf, const char *path);

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *extract_to(const char *file, off_t sz, const dir_elem_t *entry_p,
                                int oper)
{
        ENTER_("%s", file);

        int out_pipe[2] = {-1, -1};

        if (pipe(out_pipe) != 0)      /* make a pipe */
                return MAP_FAILED;

        printd(3, "Extracting %llu bytes resident in %s\n", sz, entry_p->rar_p);
        pid_t pid = fork();
        if (pid == 0) {
                int ret;
                close(out_pipe[0]);
                ret = extract_rar(entry_p->rar_p, file, entry_p->password_p,
                                        NULL, (void *)(uintptr_t) out_pipe[1]);
                close(out_pipe[1]);
                _exit(ret);
        } else if (pid < 0) {
                close(out_pipe[0]);
                close(out_pipe[1]);
                /* The fork failed. Report failure. */
                return MAP_FAILED;
        }

        close(out_pipe[1]);

        FILE *tmp = NULL;
        char *buffer = malloc(sz);
        if (!buffer)
                return MAP_FAILED;

        if (oper == E_TO_TMP)
                tmp = tmpfile();

        off_t off = 0;
        ssize_t n;
        do {
                /* read from pipe into buffer */
                n = read(out_pipe[0], buffer + off, sz - off);
                if (n == -1) {
                        if (errno == EINTR)
                                continue;
                        perror("read");
                        break;
                }
                off += n;
        } while (n && off != sz);

        /* Sync */
        if (waitpid(pid, NULL, 0) == -1) {
                /*
                 * POSIX.1-2001 specifies that if the disposition of
                 * SIGCHLD is set to SIG_IGN or the SA_NOCLDWAIT flag
                 * is set for SIGCHLD (see sigaction(2)), then
                 * children that terminate do not become zombies and
                 * a call to wait() or waitpid() will block until all
                 * children have terminated, and then fail with errno
                 * set to ECHILD.
                 */
                if (errno != ECHILD)
                        perror("waitpid");
        } 

        /* Check for incomplete buffer error */
        if (off != sz) {
                free(buffer);
                return MAP_FAILED;
        }

        printd(4, "Read %llu bytes from PIPE %d\n", off, out_pipe[0]);
        close(out_pipe[0]);

        if (tmp) {
                if (!fwrite(buffer, sz, 1, tmp)) {
                        fclose(tmp);
                        tmp = MAP_FAILED;
                } else
                        fseeko(tmp, 0, SEEK_SET);
                free(buffer);
                return tmp;
        }

        return buffer;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/

#define UNRAR_ unrar_path, unrar_path

static FILE *popen_(const dir_elem_t *entry_p, pid_t *cpid, void **mmap_addr,
                FILE **mmap_fp, int *mmap_fd)
{
        char *maddr = MAP_FAILED;
        FILE *fp = NULL;
        int fd = -1;
        int pfd[2] = {-1,};
#ifdef ENABLE_OBSOLETE_ARGS
        char *unrar_path = OPT_STR(OPT_KEY_UNRAR_PATH, 0);
#endif
        if (entry_p->flags.mmap) {
                fd = open(entry_p->rar_p, O_RDONLY);
                if (fd == -1) {
                        perror("open");
                        goto error;
                }

                if (entry_p->flags.mmap == 2) {
#ifdef HAVE_FMEMOPEN
                        maddr = extract_to(entry_p->file_p, entry_p->msize,
                                                entry_p, E_TO_MEM);
                        if (maddr != MAP_FAILED) {
                                fp = fmemopen(maddr, entry_p->msize, "r");
                                if (fp == NULL) {
                                        perror("fmemopen");
                                        goto error;
                                }
                        }
#else
                        fp = extract_to(entry_p->file_p, entry_p->msize,
                                                entry_p, E_TO_TMP);
                        if (fp == MAP_FAILED) {
                                printd(1, "Extract to tmpfile failed\n");
                                goto error;
                        }
#endif
                } else {
#if defined ( HAVE_FMEMOPEN ) && defined ( HAVE_MMAP )
                        maddr = mmap(0, P_ALIGN_(entry_p->msize), PROT_READ,
                                                MAP_SHARED, fd, 0);
                        if (maddr != MAP_FAILED) {
                                fp = fmemopen(maddr + entry_p->offset,
                                                        entry_p->msize -
                                                        entry_p->offset, "r");
                        } else {
                                perror("mmap");
                                goto error;
                        }
#else
                        fp = fopen(entry_p->rar_p, "r");
                        if (fp)
                                fseeko(fp, entry_p->offset, SEEK_SET);
                        else
                                goto error;
#endif
                }

                *mmap_addr = maddr;
                *mmap_fp = fp;
                *mmap_fd = fd;
        }

        pid_t pid;
        if (pipe(pfd) == -1) {
                perror("pipe");
                goto error;
        }

        pid = fork();
        if (pid == 0) {
                int ret;
                setpgid(getpid(), 0);
                close(pfd[0]);  /* Close unused read end */

#ifdef ENABLE_OBSOLETE_ARGS
                /* This is the child process.  Execute the shell command. */
                if (unrar_path && !entry_p->flags.mmap) {
                        fflush(stdout);
                        dup2(pfd[1], STDOUT_FILENO);
                        /* anything sent to stdout should now go down the pipe */
                        if (entry_p->password_p) {
                                char parg[MAXPASSWORD + 2];
                                printd(3, parg, MAXPASSWORD + 2, "%s%s", "-p",
                                        entry_p->password_p);
                                execl(UNRAR_, parg, "P", "-inul", entry_p->rar_p,
                                                entry_p->file_p, NULL);
                        } else {
                                execl(UNRAR_, "P", "-inul", entry_p->rar_p,
                                                entry_p->file_p, NULL);
                        }
                        /* Should never come here! */
                        _exit(EXIT_FAILURE);
                }
#endif
                ret = extract_rar(entry_p->rar_p,
                                  entry_p->flags.mmap
                                        ? entry_p->file2_p
                                        : entry_p->file_p,
                                  entry_p->password_p, fp,
                                  (void *)(uintptr_t) pfd[1]);
                close(pfd[1]);
                _exit(ret);
        } else if (pid < 0) {
                /* The fork failed. */
                goto error;
        }

        /* This is the parent process. */
        close(pfd[1]);          /* Close unused write end */
        *cpid = pid;
        return fdopen(pfd[0], "r");

error:
        if (maddr != MAP_FAILED) {
                if (entry_p->flags.mmap == 1)
                        munmap(maddr, P_ALIGN_(entry_p->msize));
                else
                        free(maddr);
        }
        if (fp)
                fclose(fp);
        if (fd >= 0)
                close(fd);
        if (pfd[0] >= 0)
                close(pfd[0]);
        if (pfd[1] >= 0)
                close(pfd[1]);

        return NULL;
}

#undef UNRAR_

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int pclose_(FILE *fp, pid_t pid)
{
        pid_t wpid;
        int status = 0;

        fclose(fp);
        killpg(pid, SIGKILL);

        /* Sync */
        do {
                wpid = waitpid(pid, &status, WNOHANG | WUNTRACED);
                if (wpid == -1) {
                        /*
                         * POSIX.1-2001 specifies that if the disposition of
                         * SIGCHLD is set to SIG_IGN or the SA_NOCLDWAIT flag
                         * is set for SIGCHLD (see sigaction(2)), then
                         * children that terminate do not become zombies and
                         * a call to wait() or waitpid() will block until all
                         * children have terminated, and then fail with errno
                         * set to ECHILD.
                         */
                        if (errno != ECHILD)
                                perror("waitpid");
                        return 0;
                }
        } while (!wpid || (!WIFEXITED(status) && !WIFSIGNALED(status)));
        if (WIFEXITED(status))
                return WEXITSTATUS(status);
        return 0;
}

/* Size of file in first volume number in which it exists */
#define VOL_FIRST_SZ op->entry_p->vsize_first

/* 
 * Size of file in the following volume numbers (if situated in more than one).
 * Compared to VOL_FIRST_SZ this is a qualified guess value only and it is
 * not uncommon that it actually becomes equal to VOL_FIRST_SZ.
 *   For reference VOL_NEXT_SZ is basically the end of file data offset in
 * first volume file reduced by the size of applicable headers and meta
 * data. For the last volume file this value is more than likely bogus.
 * This does not matter since the total file size is still reported 
 * correctly and anything trying to read it should stop once reaching EOF. 
 * The read function will infact verify that this is always the case and 
 * throw an error if trying to read beyond EOF. The important thing here
 * is that the volumes files other than the first and last match this value.
 */
#define VOL_NEXT_SZ op->entry_p->vsize_next

/* Size of file data in first volume number */
#define VOL_REAL_SZ op->entry_p->vsize_real

/* Calculate volume number base offset using cached file offsets */
#define VOL_NO(off)\
        (off < VOL_FIRST_SZ ? 0 : ((offset - VOL_FIRST_SZ) / VOL_NEXT_SZ) + 1)

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static char *get_vname(int t, const char *str, int vol, int len, int pos)
{
        ENTER_("%s   vol=%d, len=%d, pos=%d", str, vol, len, pos);
        char *s = strdup(str);
        if (!vol)
                return s;
        if (t) {
                char f[16];
                char f1[16];
                sprintf(f1, "%%0%dd", len);
                sprintf(f, f1, vol);
                strncpy(&s[pos], f, len);
        } else {
                char f[16];
                int lower = s[pos - 1] >= 'r';
                if (vol == 1) {
                        sprintf(f, "%s", (lower ? "ar" : "AR"));
                } else if (vol <= 101) {
                        sprintf(f, "%02d", (vol - 2));
                }
                /* Possible, but unlikely */
                else {
                        sprintf(f, "%c%02d", lower ? 'r' : 'R' + (vol - 2) / 100,
                                                (vol - 2) % 100);

                        --pos;
                        ++len;
                }
                strncpy(&s[pos], f, len);
        }
        return s;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lread_raw(char *buf, size_t size, off_t offset,
                struct fuse_file_info *fi)
{
        size_t n = 0;
        struct io_context *op = FH_TOCONTEXT(fi->fh);

        op->seq++;

        printd(3, "PID %05d calling %s(), seq = %d, offset=%llu\n", getpid(),
               __func__, op->seq, offset);

        size_t chunk;
        int tot = 0;
        int force_seek = 0;

        /*
         * Handle the case when a user tries to read outside file size.
         * This is especially important to handle here since last file in a
         * volume usually is of much less size than the others and conseqently
         * the chunk based calculation will not detect this.
         */
        if ((off_t)(offset + size) >= op->entry_p->stat.st_size) {
                if (offset > op->entry_p->stat.st_size)
                        return 0;       /* EOF */
                size = op->entry_p->stat.st_size - offset;
        }

        while (size) {
                FILE *fp;
                off_t src_off = 0;
                struct vol_handle *vol_p = NULL;
                if (VOL_FIRST_SZ) {
                        chunk = offset < VOL_FIRST_SZ
                                ? VOL_FIRST_SZ - offset
                                : (VOL_NEXT_SZ) -
                                  ((offset - VOL_FIRST_SZ) % (VOL_NEXT_SZ));

                        /* keep current open file */
                        int vol = VOL_NO(offset);
                        if (vol != op->vno) {
                                /* close/open */
                                op->vno = vol;
                                if (op->volHdl && op->volHdl[vol].fp) {
                                        vol_p = &op->volHdl[vol];
                                        fp = vol_p->fp;
                                        src_off = VOL_REAL_SZ - chunk;
                                        if (src_off != vol_p->pos)
                                                force_seek = 1;
                                        goto seek_check;
                                }
                                /*
                                 * It is advisable to return 0 (EOF) here
                                 * rather than -errno at failure. Some media
                                 * players tend to react "better" on that and
                                 * terminate playback as expected.
                                 */
                                char *tmp =
                                    get_vname(op->entry_p->vtype,
                                                op->entry_p->rar_p,
                                                op->vno + op->entry_p->vno_base,
                                                op->entry_p->vlen,
                                                op->entry_p->vpos);
                                if (tmp) {
                                        printd(3, "Opening %s\n", tmp);
                                        fp = fopen(tmp, "r");
                                        free(tmp);
                                        if (fp == NULL) {
                                                perror("open");
                                                return 0;       /* EOF */
                                        }
                                        fclose(op->fp);
                                        op->fp = fp;
                                        force_seek = 1;
                                } else {
                                        return 0;               /* EOF */
                                }
                        } else {
                                if (op->volHdl && op->volHdl[vol].fp)
                                        fp = op->volHdl[vol].fp;
                                else
                                        fp = op->fp;
                        }
seek_check:
                        if (force_seek || offset != op->pos) {
                                src_off = VOL_REAL_SZ - chunk;
                                printd(3, "SEEK src_off = %llu, "
                                                "VOL_REAL_SZ = %llu\n",
                                                src_off, VOL_REAL_SZ);
                                fseeko(fp, src_off, SEEK_SET);
                                force_seek = 0;
                        }
                        printd(3, "size = %zu, chunk = %zu\n", size, chunk);
                        chunk = size < chunk ? size : chunk;
                } else {
                        fp = op->fp;
                        chunk = size;
                        if (!offset || offset != op->pos) {
                                src_off = offset + op->entry_p->offset;
                                printd(3, "SEEK src_off = %llu\n", src_off);
                                fseeko(fp, src_off, SEEK_SET);
                        }
                }
                n = fread(buf, 1, chunk, fp);
                printd(3, "Read %d bytes from vol=%d, base=%d\n", n, op->vno,
                       op->entry_p->vno_base);
                if (n != chunk) {
                        size = n;
                }

                size -= n;
                offset += n;
                buf += n;
                tot += n;
                op->pos = offset;
                if (vol_p)
                        vol_p->pos += n;
        }
        return tot;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void sync_thread_read(int *pfd1, int *pfd2)
{
       do {
                errno = 0;
                WAKE_THREAD(pfd1, 1);
                WAIT_THREAD(pfd2);
        } while (errno == EINTR);
}

static void sync_thread_noread(int *pfd1, int *pfd2)
{
        do {
                errno = 0;
                WAKE_THREAD(pfd1, 2);
                WAIT_THREAD(pfd2);
        } while (errno == EINTR);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lread_rar_idx(char *buf, size_t size, off_t offset,
                struct io_context *op)
{
        int res;
        off_t o = op->buf->idx.data_p->head.offset;
        size_t s = op->buf->idx.data_p->head.size;
        off_t off = (offset - o);

        if (off >= (off_t)s)
                return -EIO;

        size = (off + size) > s
                ? size - ((off + size) - s)
                : size;
        printd(3, "Copying %zu bytes from preloaded offset @ %llu\n",
                                                size, offset);
        if (op->buf->idx.mmap) {
                memcpy(buf, op->buf->idx.data_p->bytes + off, size);
                return size;
        }
        res = pread(op->buf->idx.fd, buf, size, off + sizeof(struct idx_head));
        if (res == -1)
                return -errno;
        return res;
}

#ifdef DEBUG_READ
/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void dump_buf(int seq, FILE *fp, char *buf, off_t offset, size_t size)
{
        int i;
        char out[128];
        char *tmp = out;
        size_t size_saved = size;

        memset(out, 0, 128);
        fprintf(fp, "seq=%d offset: %llu   size: %zu   buf: %p", seq, offset, size, buf);
        size = size > 64 ? 64 : size;
        if (fp) {
                for (i = 0; i < size; i++) {
                        if (!i || !(i % 10)) {
                                sprintf(tmp, "\n%016llx : ", offset + i);
                                tmp += 20;
                        }
                        sprintf(tmp, "%02x ", (uint32_t)*(buf+i) & 0xff);
                        tmp += 3;
                        if (i && !(i % 10)) {
                                fprintf(fp, "%s", out);
                                tmp = out;
                        }
                }
                if (i % 10)
                        fprintf(fp, "%s\n", out);

                if (size_saved >= 128) {
                        buf = buf + size_saved - 64;
                        offset = offset + size_saved - 64;
                        tmp = out;

                        fprintf(fp, "\nlast 64 bytes:");
                        for (i = 0; i < 64; i++) {
                                if (!i || !(i % 10)) {
                                        sprintf(tmp, "\n%016llx : ", offset + i);
                                        tmp += 20;
                                }
                                sprintf(tmp, "%02x ", (uint32_t)*(buf+i) & 0xff);
                                tmp += 3;
                                if (i && !(i % 10)) {
                                        fprintf(fp, "%s", out);
                                        tmp = out;
                                }
                        }
                        if (i % 10)
                                fprintf(fp, "%s\n", out);
                }
                fprintf(fp, "\n");
                fflush(fp);
        }
}
#endif

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lread_rar(char *buf, size_t size, off_t offset,
                struct fuse_file_info *fi)
{
        int n = 0;
        struct io_context* op = FH_TOCONTEXT(fi->fh);

#ifdef DEBUG_READ
        char *buf_saved = buf;
        off_t offset_saved = offset;
#endif

        op->seq++;

        printd(3,
               "PID %05d calling %s(), seq = %d, size=%zu, offset=%llu/%llu\n",
               getpid(), __func__, op->seq, size, offset, op->pos);

        if ((off_t)(offset + size) >= op->entry_p->stat.st_size) {
                size = offset < op->entry_p->stat.st_size
                        ? op->entry_p->stat.st_size - offset
                        : 0;    /* EOF */
        }
        if (!size)
                goto out;

        /* Check for exception case */
        if (offset != op->pos) {
check_idx:
                if (op->buf->idx.data_p != MAP_FAILED &&
                                offset >= op->buf->idx.data_p->head.offset) {
                        n = lread_rar_idx(buf, size, offset, op);
                        goto out;
                }
                /* Check for backward read */
                if (offset < op->pos) {
                        printd(3, "seq=%d    history access    offset=%llu"
                                                " size=%zu  op->pos=%llu"
                                                "  split=%d\n",
                                                op->seq,offset,size,
                                                op->pos,
                                                (offset + size) > op->pos);
                        if ((uint32_t)(op->pos - offset) <= IOB_HIST_SZ) {
                                size_t pos = offset & (IOB_SZ-1);
                                size_t chunk = (off_t)(offset + size) > op->pos
                                        ? (size_t)(op->pos - offset)
                                        : size;
                                size_t tmp = copyFrom(buf, op->buf, chunk, pos);
                                size -= tmp;
                                buf += tmp;
                                offset += tmp;
                                n += tmp;
                        } else {
                                printd(1, "%s: Input/output error   offset=%llu"
                                                        "  pos=%llu\n",
                                                        __func__,
                                                        offset, op->pos);
                                n = -EIO;
                                goto out;
                        }
                /*
                 * Early reads at offsets reaching the last few percent of the
                 * file is most likely a request for index information.
                 */
                } else if ((((offset - op->pos) / (op->entry_p->stat.st_size * 1.0) * 100) > 95.0 &&
                                op->seq < 10)) {
                        printd(3, "seq=%d    long jump hack1    offset=%llu,"
                                                " size=%zu, buf->offset=%llu\n",
                                                op->seq, offset, size,
                                                op->buf->offset);
                        op->seq--;      /* pretend it never happened */

                        /*
                         * If enabled, attempt to extract the index information
                         * based on the offset. If that fails fall-back to best
                         * effort. That is, return all zeros according to size.
                         * In the latter case also force direct I/O since
                         * otherwise the fake data might propagate incorrectly
                         * to sub-sequent reads.
                         */
                        dir_elem_t *e_p; /* "real" cache entry */
                        if (op->entry_p->flags.save_eof) {
                                pthread_mutex_lock(&file_access_mutex);
                                e_p = cache_path_get(op->entry_p->name_p);
                                if (e_p)
                                        e_p->flags.save_eof = 0;
                                pthread_mutex_unlock(&file_access_mutex);
                                op->entry_p->flags.save_eof = 0; 
                                if (!extract_index(op->entry_p, offset)) {
                                        if (!preload_index(op->buf, op->entry_p->name_p)) {
                                                op->seq++;
                                                goto check_idx;
                                        }
                                }
                        }
                        fi->direct_io = 1;
                        pthread_mutex_lock(&file_access_mutex);
                        e_p = cache_path_get(op->entry_p->name_p);
                        if (e_p)
                                e_p->flags.direct_io = 1;
                        pthread_mutex_unlock(&file_access_mutex);
                        op->entry_p->flags.direct_io = 1;
                        memset(buf, 0, size);
                        n += size;
                        goto out;
                }
        }

        /*
         * Check if we need to wait for data to arrive.
         * This should not be happening frequently. If it does it is an
         * indication that the I/O buffer is set too small.
         */
        if ((off_t)(offset + size) > op->buf->offset)
                sync_thread_read(op->pfd1, op->pfd2);
        if ((off_t)(offset + size) > op->buf->offset) {
                if (offset >= op->buf->offset) {
                        /*
                         * This is another hack! At this point an early read
                         * far beyond the current stream position is most
                         * likely bogus. We can not blindly take the jump here
                         * since it would render the stream completely useless
                         * for continued playback. If the jump is too far off,
                         * again fall-back to best effort. Also making sure
                         * direct I/O is forced from now on to not cause any
                         * fake data to propagate in sub-sequent reads.
                         * This case is very likely for multi-part AVI 2.0.
                         */
                        if (op->seq < 15 && ((offset + size) - op->buf->offset)
                                        > (IOB_SZ - IOB_HIST_SZ)) {
                                dir_elem_t *e_p; /* "real" cache entry */ 
                                printd(3, "seq=%d    long jump hack2    offset=%llu,"
                                                " size=%zu, buf->offset=%llu\n",
                                                op->seq, offset, size,
                                                op->buf->offset);
                                op->seq--;      /* pretend it never happened */
                                fi->direct_io = 1;
                                pthread_mutex_lock(&file_access_mutex);
                                e_p = cache_path_get(op->entry_p->name_p);
                                if (e_p)
                                        e_p->flags.direct_io = 1;
                                pthread_mutex_unlock(&file_access_mutex);
                                op->entry_p->flags.direct_io = 1;
                                memset(buf, 0, size);
                                n += size;
                                goto out;
                        }
                }

                pthread_mutex_lock(&op->mutex);
                if (!op->terminate) {   /* make sure thread is running */
                        pthread_mutex_unlock(&op->mutex);
                        /* Take control of reader thread */
                        sync_thread_noread(op->pfd1, op->pfd2); /* XXX really not needed due to call above */
                        while (!feof(op->fp) &&
                                        offset > op->buf->offset) {
                                /* consume buffer */
                                op->pos += op->buf->used;
                                op->buf->ri = op->buf->wi;
                                op->buf->used = 0;
                                (void)readTo(op->buf, op->fp,
                                                IOB_SAVE_HIST);
                                sched_yield();
                        }

                        if (!feof(op->fp)) {
                                op->buf->ri = offset & (IOB_SZ - 1);
                                op->buf->used -= (offset - op->pos);
                                op->pos = offset;

                                /* Pull in rest of data if needed */
                                if ((size_t)(op->buf->offset - offset) < size)
                                        (void)readTo(op->buf, op->fp,
                                                        IOB_SAVE_HIST);
                        }
                } else {
                        pthread_mutex_unlock(&op->mutex);
                }
        }

        if (size) {
                int off = offset - op->pos;
                n += readFrom(buf, op->buf, size, off);
                op->pos += (off + size);
                if (!op->terminate)
                        WAKE_THREAD(op->pfd1, 0);
        }

out:

#ifdef DEBUG_READ
        if (n > 0)
                dump_buf(op->seq, op->dbg_fp, buf_saved, offset_saved, n);
#endif

        printd(3, "%s: RETURN %d\n", __func__, n);
        return n;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lflush(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", path);
        (void)path;             /* touch */
        (void)fi;               /* touch */
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lrelease(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", path);

        (void)path;             /* touch */
        close(FH_TOFD(fi->fh));
        if (FH_TOENTRY(fi->fh))
                filecache_freeclone(FH_TOENTRY(fi->fh));
        free(FH_TOIO(fi->fh));
        FH_ZERO(fi->fh);
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lread(const char *path, char *buffer, size_t size, off_t offset,
                struct fuse_file_info *fi)
{
        int res;

        ENTER_("%s   size = %zu, offset = %llu", path, size, offset);

        (void)path;             /* touch */

        res = pread(FH_TOFD(fi->fh), buffer, size, offset);
        if (res == -1)
                return -errno;
        return res;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lopen(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", path);
        int fd = open(path, fi->flags);
        if (fd == -1)
                return -errno;
        struct io_handle *io = malloc(sizeof(struct io_handle));
        if (!io) {
                close(fd);
                return -EIO;
        }
        FH_SETIO(fi->fh, io);
        FH_SETTYPE(fi->fh, IO_TYPE_NRM);
        FH_SETENTRY(fi->fh, NULL);
        FH_SETFD(fi->fh, fd);
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/

#if !defined ( DEBUG_ ) || DEBUG_ < 5
#define dump_stat(s)
#else
#define DUMP_STATO_(m) \
        fprintf(stderr, "%10s = %o (octal)\n", #m , (unsigned int)stbuf->m)
#define DUMP_STAT4_(m) \
        fprintf(stderr, "%10s = %u\n", #m , (unsigned int)stbuf->m)
#define DUMP_STAT8_(m) \
        fprintf(stderr, "%10s = %llu\n", #m , (unsigned long long)stbuf->m)

static void dump_stat(struct stat *stbuf)
{
        fprintf(stderr, "struct stat {\n");
        DUMP_STAT4_(st_dev);
        DUMP_STATO_(st_mode);
        DUMP_STAT4_(st_nlink);
        if (sizeof(stbuf->st_ino) > 4)
                DUMP_STAT8_(st_ino);
        else
                DUMP_STAT4_(st_ino);
        DUMP_STAT4_(st_uid);
        DUMP_STAT4_(st_gid);
        DUMP_STAT4_(st_rdev);
        if (sizeof(stbuf->st_size) > 4)
                DUMP_STAT8_(st_size);
        else
                DUMP_STAT4_(st_size);
#ifdef HAVE_STRUCT_STAT_ST_BLOCKS
        DUMP_STAT4_(st_blocks);
#endif
#ifdef HAVE_STRUCT_STAT_ST_BLKSIZE
        DUMP_STAT4_(st_blksize);
#endif
#ifdef HAVE_STRUCT_STAT_ST_GEN
        DUMP_STAT4_(st_gen);
#endif
        fprintf(stderr, "}\n");
}

#undef DUMP_STAT4_
#undef DUMP_STAT8_
#endif

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int collect_files(const char *arch, struct dir_entry_list *list)
{
        RAROpenArchiveDataEx d;
        int files = 0;

        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = strdup(arch);
        d.OpenMode = RAR_OM_LIST;

        while (1) {
                HANDLE hdl = RAROpenArchiveEx(&d);
                if (d.OpenResult)
                        break;
                if (!(d.Flags & MHD_VOLUME)) {
                        files = 1;
                        list = dir_entry_add(list, d.ArcName, NULL, DIR_E_NRM);
                        break;
                }
                if (!files && !(d.Flags & MHD_FIRSTVOLUME))
                        break;

                ++files;
                list = dir_entry_add(list, d.ArcName, NULL, DIR_E_NRM);
                RARCloseArchive(hdl);
                RARNextVolumeName(d.ArcName, !(d.Flags & MHD_NEWNUMBERING));
        }
        free(d.ArcName);
        return files;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int is_rxx_vol(const char *name)
{
        size_t len = strlen(name);
        if (name[len - 4] == '.' && 
                        (name[len - 3] >= 'r' || name[len - 3] >= 'R') &&
                        isdigit(name[len - 2]) && isdigit(name[len - 1])) {
                /* This seems to be a classic .rNN rar volume file.
                 * Let the rar header be the final judge. */
                return 1;
        }
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int get_vformat(const char *s, int t, int *l, int *p)
{
        int len = 0;
        int pos = 0;
        int vol = 0;
        const size_t SLEN = strlen(s);
        if (t) {
                int dot = 0;
                len = SLEN - 1;
                while (dot < 2 && len >= 0) {
                        if (s[len--] == '.')
                                ++dot;
                }
                if (len >= 0) {
                        pos = len + 1;
                        len = SLEN - pos;
                        if (len >= 10) {
                                if ((s[pos + 1] == 'p' || s[pos + 1] == 'P') &&
                                    (s[pos + 2] == 'a' || s[pos + 2] == 'A') &&
                                    (s[pos + 3] == 'r' || s[pos + 3] == 'R') &&
                                    (s[pos + 4] == 't' || s[pos + 4] == 'T')) {
                                        pos += 5;       /* - ".part" */
                                        len -= 9;       /* - ".ext" */
                                        vol = strtoul(&s[pos], NULL, 10);
                                } 
                        }
                }
        } else {
                int dot = 0;
                len = SLEN - 1;
                while (dot < 1 && len >= 0) {
                        if (s[len--] == '.')
                                ++dot;
                }
                if (len >= 0) {
                        pos = len + 1;
                        len = SLEN - pos;
                        if (len == 4) {
                                pos += 2;
                                len -= 2;
                                if ((s[pos - 1] == 'r' || s[pos - 1] == 'R') &&
                                    (s[pos    ] == 'a' || s[pos    ] == 'A') &&
                                    (s[pos + 1] == 'r' || s[pos + 1] == 'R')) {
                                        vol = 1;
                                } else {
                                        int lower = s[pos - 1] >= 'r';
                                        errno = 0;
                                        vol = strtoul(&s[pos], NULL, 10) + 2 +
                                                /* Possible, but unlikely */
                                                (100 * (s[pos - 1] - (lower ? 'r' : 'R')));
                                        vol = errno ? 0 : vol;
                                }
                        }
                }
        }
        if (l) *l = vol ? len : 0;
        if (p) *p = vol ? pos : 0;
        return vol ? vol : 1;
}

#define IS_AVI(s) (!strcasecmp((s)+(strlen(s)-4), ".avi"))
#define IS_MKV(s) (!strcasecmp((s)+(strlen(s)-4), ".mkv"))
#define IS_RAR(s) (!strcasecmp((s)+(strlen(s)-4), ".rar"))
#define IS_CBR(s) (!OPT_SET(OPT_KEY_NO_EXPAND_CBR) && \
                        !strcasecmp((s)+(strlen(s)-4), ".cbr"))
#define IS_RXX(s) (is_rxx_vol(s))
#if 0
#define IS_RAR_DIR(l) \
        ((l)->HostOS != HOST_UNIX && (l)->HostOS != HOST_BEOS \
                ? (l)->FileAttr & 0x10 : (l)->FileAttr & S_IFDIR)
#else
/* Unless we need to support RAR version < 2.0 this is good enough */
#define IS_RAR_DIR(l) \
        (((l)->Flags&LHD_DIRECTORY)==LHD_DIRECTORY)
#endif
#define GET_RAR_MODE(l) \
        ((l)->HostOS != HOST_UNIX && (l)->HostOS != HOST_BEOS \
                ? IS_RAR_DIR(l) ? (S_IFDIR|(0777&~umask_)) \
                : (S_IFREG|(0666&~umask_)) : (l)->FileAttr)
#define GET_RAR_SZ(l) \
        (IS_RAR_DIR(l) ? 4096 : (((l)->UnpSizeHigh * 0x100000000ULL) | \
                (l)->UnpSize))
#define GET_RAR_PACK_SZ(l) \
        (IS_RAR_DIR(l) ? 4096 : (((l)->PackSizeHigh * 0x100000000ULL) | \
                (l)->PackSize))

/*!
 ****************************************************************************
 *
 ****************************************************************************/
static int CALLBACK index_callback(UINT msg, LPARAM UserData,
                LPARAM P1, LPARAM P2)
{
        struct eof_data *eofd = (struct eof_data *)UserData;

        if (msg == UCM_PROCESSDATA) {
                /*
                 * We do not need to handle the case that not all data is
                 * written after return from write() since the pipe is not
                 * opened using the O_NONBLOCK flag.
                 */
                if (eofd->coff != eofd->toff) {
                        eofd->coff += P2;
                        if (eofd->coff > eofd->toff) {
                                off_t delta = eofd->coff - eofd->toff;
                                if (delta < P2) {
                                        eofd->coff -= delta;
                                        P1 = (LPARAM)((void*)P1 + (P2 - delta));
                                        P2 = delta;
                                }
                        }
                }
                if (eofd->coff == eofd->toff) {
                        eofd->size += P2;
                        NO_UNUSED_RESULT write(eofd->fd, (char *)P1, P2);
                        fdatasync(eofd->fd);      /* XXX needed!? */
                        eofd->toff += P2;
                        eofd->coff = eofd->toff;
                }

        }
        if (msg == UCM_CHANGEVOLUME)
                return access((char *)P1, F_OK);

        return 1;
}

/*!
 ****************************************************************************
 *
 ****************************************************************************/
static int extract_index(const dir_elem_t *entry_p, off_t offset)
{
        int e = ERAR_BAD_DATA;
        struct RAROpenArchiveData d =
                {entry_p->rar_p, RAR_OM_EXTRACT, ERAR_EOPEN, NULL, 0, 0, 0};
        struct RARHeaderData header;
        HANDLE hdl = 0;
        struct idx_head head = {R2I_MAGIC, 0, 0, 0, 0};
        char *r2i;

        struct eof_data eofd;
        eofd.toff = offset;
        eofd.coff = 0;
        eofd.size = 0;

        ABS_ROOT(r2i, entry_p->name_p);
        strcpy(&r2i[strlen(r2i) - 3], "r2i");

        eofd.fd = open(r2i, O_WRONLY|O_CREAT|O_EXCL,
                        S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
        if (eofd.fd == -1)
                goto index_error;
        lseek(eofd.fd, sizeof(struct idx_head), SEEK_SET);

        hdl = RAROpenArchive(&d);
        if (!hdl || d.OpenResult)
                goto index_error;

        if (entry_p->password_p && strlen(entry_p->password_p))
                RARSetPassword(hdl, entry_p->password_p);

        header.CmtBufSize = 0;
        RARSetCallback(hdl, index_callback, (LPARAM)&eofd);
        while (1) {
                if (RARReadHeader(hdl, &header))
                        break;
                /* We won't extract subdirs */
                if (IS_RAR_DIR(&header) ||
                        strcmp(header.FileName, entry_p->file_p)) {
                                if (RARProcessFile(hdl, RAR_SKIP, NULL, NULL))
                                        break;
                } else {
                        e = RARProcessFile(hdl, RAR_TEST, NULL, NULL);
                        if (!e) {
                                head.offset = offset;
                                head.size = eofd.size;
                                lseek(eofd.fd, (off_t)0, SEEK_SET);
                                NO_UNUSED_RESULT write(eofd.fd, (void*)&head,
                                                sizeof(struct idx_head));
                        }
                        break;
                }
        }

index_error:
        if (eofd.fd != -1)
                close(eofd.fd);
        if (hdl)
                RARCloseArchive(hdl);
        return e ? -1 : 0;
}

/*!
 ****************************************************************************
 *
 ****************************************************************************/
static int CALLBACK extract_callback(UINT msg, LPARAM UserData,
                LPARAM P1, LPARAM P2)
{
        if (msg == UCM_PROCESSDATA) {
                /*
                 * We do not need to handle the case that not all data is
                 * written after return from write() since the pipe is not
                 * opened using the O_NONBLOCK flag.
                 */
                int fd = UserData ? UserData : STDOUT_FILENO;
                if (write(fd, (void *)P1, P2) == -1) {
                        /*
                         * Do not treat EPIPE as an error. It is the normal
                         * case when the process is terminted, ie. the pipe is
                         * closed since SIGPIPE is not handled.
                         */
                        if (errno != EPIPE)
                                perror("write");
                        return -1;
                }
        }
        if (msg == UCM_CHANGEVOLUME)
                return access((char *)P1, F_OK);

        return 1;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int extract_rar(char *arch, const char *file, char *passwd, FILE *fp,
                void *arg)
{
        int ret = 0;
        struct RAROpenArchiveData d = {
                arch, RAR_OM_EXTRACT, ERAR_EOPEN, NULL, 0, 0, 0 };
        struct RARHeaderData header;
        HANDLE hdl = fp ? RARInitArchive(&d, fp) : RAROpenArchive(&d);
        if (!hdl || d.OpenResult)
                goto extract_error;

        if (passwd && strlen(passwd))
                RARSetPassword(hdl, passwd);

        header.CmtBufSize = 0;

        RARSetCallback(hdl, extract_callback, (LPARAM) (arg));
        while (1) {
                if (RARReadHeader(hdl, &header))
                        break;

                /* We won't extract subdirs */
                if (IS_RAR_DIR(&header) || strcmp(header.FileName, file)) {
                        if (RARProcessFile(hdl, RAR_SKIP, NULL, NULL))
                                break;
                } else {
                        ret = RARProcessFile(hdl, RAR_TEST, NULL, NULL);
                        break;
                }
        }

extract_error:

        if (!fp)
                RARCloseArchive(hdl);
        else
                RARFreeArchive(hdl);
        return ret;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
#ifdef USE_RAR_PASSWORD
static char *get_password(const char *file, char *buf)
{
        if (file && !OPT_SET(OPT_KEY_NO_PASSWD)) {
                size_t l = strlen(file);
                char *F = alloca(l + 1);
                strcpy(F, file);
                strcpy(F + (l - 4), ".pwd");
                FILE *fp = fopen(F, "r");
                if (fp) {
                        char FMT[8];
                        sprintf(FMT, "%%%ds", MAXPASSWORD);
                        NO_UNUSED_RESULT fscanf(fp, FMT, buf);
                        fclose(fp);
                        return buf;
                }
        }
        return NULL;
}
#else
#define get_password(a, b) (NULL)
#endif

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void set_rarstats(dir_elem_t *entry_p, RARArchiveListEx *alist_p,
                int force_dir)
{
        if (!force_dir) {
                mode_t mode = GET_RAR_MODE(alist_p);
                if (!S_ISDIR(mode)) {
                        /* Force file to be treated as a 'regular file' */
                        mode = (mode & ~S_IFMT) | S_IFREG;
                }
                entry_p->stat.st_mode = mode;
                entry_p->stat.st_nlink =
                        S_ISDIR(mode) ? 2 : alist_p->Method - (FHD_STORING - 1);
                entry_p->stat.st_size = GET_RAR_SZ(alist_p);
        } else {
                entry_p->stat.st_mode = (S_IFDIR | (0777 & ~umask_));
                entry_p->stat.st_nlink = 2;
                entry_p->stat.st_size = 4096;
        }
        entry_p->stat.st_uid = getuid();
        entry_p->stat.st_gid = getgid();
        entry_p->stat.st_ino = 0;

#ifdef HAVE_STRUCT_STAT_ST_BLOCKS
        /*
         * This is far from perfect but does the job pretty well!
         * If there is some obvious way to calculate the number of blocks
         * used by a file, please tell me! Most Linux systems seems to
         * apply some sort of multiple of 8 blocks scheme?
         */
        entry_p->stat.st_blocks =
            (((entry_p->stat.st_size + (8 * 512)) & ~((8 * 512) - 1)) / 512);
#endif
        struct tm t;
        memset(&t, 0, sizeof(struct tm));

        union dos_time_t {
                __extension__ struct {
#ifndef WORDS_BIGENDIAN
                        unsigned int second:5;
                        unsigned int minute:6;
                        unsigned int hour:5;
                        unsigned int day:5;
                        unsigned int month:4;
                        unsigned int year:7;
#else
                        unsigned int year:7;
                        unsigned int month:4;
                        unsigned int day:5;
                        unsigned int hour:5;
                        unsigned int minute:6;
                        unsigned int second:5;
#endif
                };

                /*
                 * Avoid type-punned pointer warning when strict aliasing is used
                 * with some versions of gcc.
                 */
                unsigned int as_uint_;
        };

        union dos_time_t *dos_time = (union dos_time_t *)&alist_p->FileTime;

        t.tm_sec = dos_time->second;
        t.tm_min = dos_time->minute;
        t.tm_hour = dos_time->hour;
        t.tm_mday = dos_time->day;
        t.tm_mon = dos_time->month - 1;
        t.tm_year = (1980 + dos_time->year) - 1900;
        entry_p->stat.st_atime = mktime(&t);
        entry_p->stat.st_mtime = entry_p->stat.st_atime;
        entry_p->stat.st_ctime = entry_p->stat.st_atime;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
#define NEED_PASSWORD() \
        ((MainHeaderFlags & MHD_PASSWORD) || (next->Flags & LHD_PASSWORD))
#define BS_TO_UNIX(p) \
        do {\
                char *s = (p); \
                while(*s++) \
                        if (*s == 92) *s = '/'; \
        } while(0)
#define CHRCMP(s, c) (!(s[0] == (c) && s[1] == '\0'))

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline size_t
wide_to_char(char *dst, const wchar_t *src, size_t size)
{
#ifdef HAVE_WCSTOMBS
        if (*src) {
                size_t n = wcstombs(NULL, src, 0);
                if (n != (size_t)-1) {
                        if (size >= (n + 1))
                                return wcstombs(dst, src, size);
                }
                /* Translation failed! Possibly a call to snprintf()
                 * using "%ls" could be added here as fall-back. */
        }
#endif
        return -1;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int listrar_rar(const char *path, struct dir_entry_list **buffer,
                const char *arch,  HANDLE hdl, const RARArchiveListEx *next,
                const dir_elem_t *entry_p)
{
        printd(3, "%llu byte RAR file %s found in archive %s\n",
               GET_RAR_PACK_SZ(next), entry_p->name_p, arch);

        RAROpenArchiveData d2;
        d2.ArcName = entry_p->name_p;
        d2.OpenMode = RAR_OM_LIST;
        d2.CmtBuf = NULL;
        d2.CmtBufSize = 0;
        d2.CmtSize = 0;
        d2.CmtState = 0;

        HANDLE hdl2 = NULL;
        FILE *fp = NULL;
        char *maddr = MAP_FAILED;
        off_t msize = 0;
        int mflags = 0;
        const unsigned int MainHeaderFlags = RARGetMainHeaderFlags(hdl);

        if (next->Method == FHD_STORING && 
                        !(MainHeaderFlags & (MHD_PASSWORD | MHD_VOLUME))) {
                struct stat st;
                int fd = fileno(RARGetFileHandle(hdl));
                if (fd == -1)
                        goto file_error;
                (void)fstat(fd, &st);
#if defined ( HAVE_FMEMOPEN ) && defined ( HAVE_MMAP )
                maddr = mmap(0, P_ALIGN_(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
                if (maddr != MAP_FAILED)
                        fp = fmemopen(maddr + (next->Offset + next->HeadSize), GET_RAR_PACK_SZ(next), "r");
#else
                fp = fopen(entry_p->rar_p, "r");
                if (fp)
                        fseeko(fp, next->Offset + next->HeadSize, SEEK_SET);
#endif
                msize = st.st_size;
                mflags = 1;
        } else {
#ifdef HAVE_FMEMOPEN
                maddr = extract_to(next->FileName, GET_RAR_SZ(next), entry_p, E_TO_MEM);
                if (maddr != MAP_FAILED)
                        fp = fmemopen(maddr, GET_RAR_SZ(next), "r");
#else
                fp = extract_to(next->FileName, GET_RAR_SZ(next), entry_p, E_TO_TMP);
                if (fp == MAP_FAILED) {
                        fp = NULL;
                        printd(1, "Extract to tmpfile failed\n");
                }
#endif
                msize = GET_RAR_SZ(next);
                mflags = 2;
        }

        if (fp)
                hdl2 = RARInitArchive(&d2, fp);
        if (!hdl2)
                goto file_error;

        RARArchiveListEx LL;
        RARArchiveListEx *next2 = &LL;
        if (!RARListArchiveEx(&hdl2, next2, NULL)) {
                hdl2 = NULL;
                goto file_error;
        }

        if (RARGetMainHeaderFlags(hdl2) & MHD_VOLUME) {
                hdl2 = NULL;
                goto file_error;
        }

        char *tmp1 = strdup(entry_p->name_p);
        char *rar_root = dirname(tmp1);
        size_t rar_root_len = strlen(rar_root);
        size_t path_len = strlen(path);
        int is_root_path = !strcmp(rar_root, path);

        while (next2) {
                dir_elem_t *entry2_p;

                if (next2->Flags & LHD_UNICODE)
                        (void)wide_to_char(next2->FileName, next2->FileNameW,
                                                sizeof(next2->FileName));
                BS_TO_UNIX(next2->FileName);

                printd(3, "File inside archive is %s\n", next2->FileName);

                /* Skip compressed image files */
                if (!OPT_SET(OPT_KEY_SHOW_COMP_IMG) &&
                                next2->Method != FHD_STORING &&
                                IS_IMG(next2->FileName)) {
                        next2 = next2->next;
                        continue;
                }

                int display = 0;
                char *rar_file;
                ABS_MP(rar_file, rar_root, next2->FileName);
                char *tmp2 = strdup(next2->FileName);
                char *rar_name = dirname(tmp2);

                if (is_root_path) {
                        if (!CHRCMP(rar_name, '.'))
                                display = 1;

                        /*
                         * Handle the rare case when the parent folder does not have
                         * its own entry in the file header. The entry needs to be
                         * faked by adding it to the cache. If the parent folder is
                         * discovered later in the header the faked entry will be
                         * invalidated and replaced with the real stats.
                         */
                        if (!display) {
                                char *safe_path = strdup(rar_name);
                                if (!strcmp(basename(safe_path), rar_name)) {
                                        char *mp;
                                        ABS_MP(mp, path, rar_name);
                                        entry2_p = cache_path_get(mp);
                                        if (entry2_p == NULL) {
                                                printd(3, "Adding %s to cache\n", mp);
                                                entry2_p = cache_path_alloc(mp);
                                                entry2_p->name_p = strdup(mp);
                                                entry2_p->rar_p = strdup(arch);
                                                entry2_p->flags.force_dir = 1;
                                                set_rarstats(entry2_p, next2, 1);
                                        }
                                        if (buffer) {
                                                *buffer = dir_entry_add_hash(
                                                        *buffer, rar_name,
                                                        &entry2_p->stat, entry2_p->dir_hash,
                                                       DIR_E_RAR);
                                        }
                                }
                                free(safe_path);
                        }
                } else {
                        if (rar_root_len < path_len)
                                if (!strcmp(path + rar_root_len + 1, rar_name))
                                        display = 1;
                }

                printd(3, "Looking up %s in cache\n", rar_file);
                entry2_p = cache_path_get(rar_file);
                if (entry2_p)  {
                        /*
                         * Check if this was a forced/fake entry. In that
                         * case update it with proper stats.
                         */
                        if (entry2_p->flags.force_dir) {
                                set_rarstats(entry2_p, next2, 0);
                                entry2_p->flags.force_dir = 0;
                        }
                        goto cache_hit;
                }

                /* Allocate a cache entry for this file */
                printd(3, "Adding %s to cache\n", rar_file);
                entry2_p = cache_path_alloc(rar_file);
                entry2_p->name_p = strdup(rar_file);
                entry2_p->rar_p = strdup(arch);
                entry2_p->file_p = strdup(next->FileName);
                entry2_p->file2_p = strdup(next2->FileName);
                entry2_p->offset = (next->Offset + next->HeadSize);
                entry2_p->flags.mmap = mflags;
                entry2_p->msize = msize;
                entry2_p->flags.multipart = 0;
                entry2_p->flags.raw = 0;        /* no raw support yet */
                entry2_p->flags.save_eof = 0;
                entry2_p->flags.direct_io = 1;
                set_rarstats(entry2_p, next2, 0);

cache_hit:

                if (display && buffer) {
                        char *safe_path = strdup(next2->FileName);
                        *buffer = dir_entry_add_hash(
                                        *buffer, basename(safe_path),
                                        &entry2_p->stat, entry2_p->dir_hash,
                                        DIR_E_RAR);
                        free(safe_path);
                }
                free(tmp2);
                next2 = next2->next;
        }
        RARFreeListEx(&LL);
        RARFreeArchive(hdl2);
        free(tmp1);

file_error:
        if (fp)
                fclose(fp);
        if (maddr != MAP_FAILED) {
                if (mflags == 1)
                        munmap(maddr, P_ALIGN_(msize));
                else
                        free(maddr);
        }

        return hdl2 ? 0 : 1;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int listrar(const char *path, struct dir_entry_list **buffer,
                const char *arch, const char *Password)
{
        ENTER_("%s   arch=%s", path, arch);

        pthread_mutex_lock(&file_access_mutex);
        RAROpenArchiveData d;
        d.ArcName = (char *)arch;       /* Horrible cast! But hey... it is the API! */
        d.OpenMode = RAR_OM_LIST;
        d.CmtBuf = NULL;
        d.CmtBufSize = 0;
        d.CmtSize = 0;
        d.CmtState = 0;
        HANDLE hdl = RAROpenArchive(&d);

        /* Check for fault */
        if (d.OpenResult) {
                pthread_mutex_unlock(&file_access_mutex);
                return d.OpenResult;
        }

        if (Password)
                RARSetPassword(hdl, (char *)Password);

        off_t FileDataEnd;
        RARArchiveListEx L;
        RARArchiveListEx *next = &L;
        if (!RARListArchiveEx(&hdl, next, &FileDataEnd)) {
                pthread_mutex_unlock(&file_access_mutex);
                return 1;
        }

        const unsigned int MainHeaderSize = RARGetMainHeaderSize(hdl);
        const unsigned int MainHeaderFlags = RARGetMainHeaderFlags(hdl);

        char *tmp1 = strdup(arch);
        char *rar_root = strdup(dirname(tmp1));
        free(tmp1);
        tmp1 = rar_root;
        rar_root += strlen(OPT_STR2(OPT_KEY_SRC, 0));
        size_t rar_root_len = strlen(rar_root);
        int is_root_path = (!strcmp(rar_root, path) || !CHRCMP(path, '/'));

        while (next) {
                if (next->Flags & LHD_UNICODE)
                        (void)wide_to_char(next->FileName, next->FileNameW,
                                                sizeof(next->FileName));
                BS_TO_UNIX(next->FileName);

                /* Skip compressed image files */
                if (!OPT_SET(OPT_KEY_SHOW_COMP_IMG) &&
                                next->Method != FHD_STORING &&
                                IS_IMG(next->FileName)) {
                        next = next->next;
                        continue;
                }

                int display = 0;
                char *tmp2 = strdup(next->FileName);
                char *rar_name = strdup(dirname(tmp2));
                free(tmp2);
                tmp2 = rar_name;

                if (is_root_path) {
                        if (!CHRCMP(rar_name, '.'))
                                display = 1;

                        /*
                         * Handle the rare case when the parent folder does not have
                         * its own entry in the file header. The entry needs to be
                         * faked by adding it to the cache. If the parent folder is
                         * discovered later in the header the faked entry will be
                         * invalidated and replaced with the real stats.
                         */
                        if (!display) {
                                char *safe_path = strdup(rar_name);
                                if (!strcmp(basename(safe_path), rar_name)) {
                                        char *mp;
                                        ABS_MP(mp, path, rar_name);
                                        dir_elem_t *entry_p = cache_path_get(mp);
                                        if (entry_p == NULL) {
                                                printd(3, "Adding %s to cache\n", mp);
                                                entry_p = cache_path_alloc(mp);
                                                entry_p->name_p = strdup(mp);
                                                entry_p->rar_p = strdup(arch);
                                                entry_p->file_p = strdup(rar_name);
                                                assert(!entry_p->password_p && "Unexpected handle");
                                                entry_p->password_p = (NEED_PASSWORD()
                                                        ? strdup(Password)
                                                        : entry_p->password_p);
                                                entry_p->flags.force_dir = 1;

                                                /* Check if part of a volume */
                                                if (MainHeaderFlags & MHD_VOLUME) {
                                                        entry_p->flags.multipart = 1;
                                                        entry_p->vtype = MainHeaderFlags & MHD_NEWNUMBERING ? 1 : 0;
                                                        /* 
                                                         * Make sure parent folders are always searched
                                                         * from the first volume file since sub-folders
                                                         * might actually be placed elsewhere.
                                                         */
                                                        RARVolNameToFirstName(entry_p->rar_p, !entry_p->vtype);
                                                } else {
                                                        entry_p->flags.multipart = 0;
                                                }
                                                set_rarstats(entry_p, next, 1);
                                        }

                                        if (buffer) {
                                                *buffer = dir_entry_add_hash(
                                                        *buffer, rar_name,
                                                        &entry_p->stat, entry_p->dir_hash,
                                                        DIR_E_RAR);
                                        }
                                }
                                free(safe_path);
                        }
                } else if (!strcmp(path + rar_root_len + 1, rar_name)) {
                        display = 1;
                }
                free(tmp2);

                char *mp;
                if (!display) {
                        ABS_MP(mp, (*rar_root ? rar_root : "/"),
                                        next->FileName);
                } else {
                        char *rar_dir = strdup(next->FileName);
                        ABS_MP(mp, path, basename(rar_dir));
                        free(rar_dir);
                }

                if (!IS_RAR_DIR(next) && OPT_SET(OPT_KEY_FAKE_ISO)) {
                        int l = OPT_CNT(OPT_KEY_FAKE_ISO)
                                        ? optdb_find(OPT_KEY_FAKE_ISO, mp)
                                        : optdb_find(OPT_KEY_IMG_TYPE, mp);
                        if (l)
                                strcpy(mp + (strlen(mp) - l), "iso");
                }

                printd(3, "Looking up %s in cache\n", mp);
                dir_elem_t *entry_p = cache_path_get(mp);
                if (entry_p)  {
                        /* 
                         * Check if this was a forced/fake entry. In that
                         * case update it with proper stats.
                         */
                        if (entry_p->flags.force_dir) {
                                set_rarstats(entry_p, next, 0);
                                entry_p->flags.force_dir = 0;
                        }
                        goto cache_hit;
                }

                /* Allocate a cache entry for this file */
                printd(3, "Adding %s to cache\n", mp);
                entry_p = cache_path_alloc(mp);
                entry_p->name_p = strdup(mp);
                entry_p->rar_p = strdup(arch);
                entry_p->file_p = strdup(next->FileName);
                assert(!entry_p->password_p && "Unexpected handle");
                entry_p->password_p = (NEED_PASSWORD()
                        ? strdup(Password)
                        : entry_p->password_p);

                /* Check for .rar file inside archive */
                if (!OPT_SET(OPT_KEY_FLAT_ONLY) && IS_RAR(entry_p->name_p)
                                        && !IS_RAR_DIR(next)) {
                        int vno = !(MainHeaderFlags & MHD_VOLUME)
                                ? 1
                                : get_vformat(arch, entry_p->vtype, NULL, NULL);
                        if (vno > 1 || !listrar_rar(path, buffer, arch, hdl, next, entry_p)) {
                                /* We are done with this rar file (.rar will never display!) */
                                inval_cache_path(mp);
                                next = next->next;
                                continue;
                        }
                }

                if (next->Method == FHD_STORING && !NEED_PASSWORD() &&
                                 !IS_RAR_DIR(next)) {
                        entry_p->flags.raw = 1;
                        if ((MainHeaderFlags & MHD_VOLUME) &&   /* volume ? */
                                        ((next->Flags & (LHD_SPLIT_BEFORE | LHD_SPLIT_AFTER)))) {
                                int len, pos;

                                entry_p->flags.multipart = 1;
                                entry_p->flags.image = IS_IMG(next->FileName);
                                entry_p->vtype = MainHeaderFlags & MHD_NEWNUMBERING ? 1 : 0;
                                entry_p->vno_base = get_vformat(arch, entry_p->vtype, &len, &pos);

                                if (len > 0) {
                                        entry_p->vlen = len;
                                        entry_p->vpos = pos;
                                        if (!IS_RAR_DIR(next)) {
                                                entry_p->vsize_real = FileDataEnd;
                                                entry_p->vsize_next = FileDataEnd - 
                                                        (SIZEOF_MARKHEAD + MainHeaderSize + next->HeadSize);
                                                entry_p->vsize_first = GET_RAR_PACK_SZ(next);
                                        }
                                } else {
                                        entry_p->flags.raw = 0;
                                        entry_p->flags.save_eof =
                                                OPT_SET(OPT_KEY_SAVE_EOF) ? 1 : 0;
                                }
                        } else {
                                entry_p->flags.multipart = 0;
                                entry_p->offset = (next->Offset + next->HeadSize);
                        }
                } else {        /* Folder or Compressed and/or Encrypted */
                        entry_p->flags.raw = 0;
                        if (!IS_RAR_DIR(next)) {
                                if (!NEED_PASSWORD())
                                        entry_p->flags.save_eof =
                                                OPT_SET(OPT_KEY_SAVE_EOF) ? 1 : 0;
                                 else
                                        entry_p->flags.save_eof = 0;
                        }
                        /* Check if part of a volume */
                        if (MainHeaderFlags & MHD_VOLUME) {
                                entry_p->flags.multipart = 1;
                                entry_p->vtype = MainHeaderFlags & MHD_NEWNUMBERING ? 1 : 0;
                                /* 
                                 * Make sure parent folders are always searched
                                 * from the first volume file since sub-folders
                                 * might actually be placed elsewhere.
                                 */
                                RARVolNameToFirstName(entry_p->rar_p, !entry_p->vtype);
                        } else {
                                entry_p->flags.multipart = 0;
                        }
                }
                set_rarstats(entry_p, next, 0);

cache_hit:

                if (display && buffer) {
                        char *safe_path = strdup(entry_p->name_p);
                        *buffer = dir_entry_add_hash(
                                        *buffer, basename(safe_path),
                                        &entry_p->stat,
                                        entry_p->dir_hash,
                                        DIR_E_RAR);
                        free(safe_path);
                }
                next = next->next;
        }

        RARFreeListEx(&L);
        RARCloseArchive(hdl);
        free(tmp1);
        pthread_mutex_unlock(&file_access_mutex);

        return 0;
}

#undef NEED_PASSWORD
#undef BS_TO_UNIX
#undef CHRCMP

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int f0(SCANDIR_ARG3 e)
{
        /*
         * We could maybe fallback to using lstat() here if
         * _DIRENT_HAVE_D_TYPE is not defined. But it is probably
         * way to slow to use in filters. That would also require
         * calls to getcwd() etc. since the current path is not
         * known in this context.
         */
#ifdef _DIRENT_HAVE_D_TYPE
        if (e->d_type != DT_UNKNOWN)
                return (!(IS_RAR(e->d_name) && e->d_type == DT_REG) &&
                        !(IS_CBR(e->d_name) && e->d_type == DT_REG) &&
                        !(IS_RXX(e->d_name) && e->d_type == DT_REG));
#endif
        return !IS_RAR(e->d_name) && !IS_CBR(e->d_name) && 
                        !IS_RXX(e->d_name);
}

static int f1(SCANDIR_ARG3 e)
{
        /*
         * We could maybe fallback to using lstat() here if
         * _DIRENT_HAVE_D_TYPE is not defined. But it is probably
         * way to slow to use in filters. That would also require
         * calls to getcwd() etc. since the current path is not
         * known in this context.
         */
#ifdef _DIRENT_HAVE_D_TYPE
        if (e->d_type != DT_UNKNOWN)
                return (IS_RAR(e->d_name) || IS_CBR(e->d_name)) && 
                                e->d_type == DT_REG;
#endif
        return IS_RAR(e->d_name) || IS_CBR(e->d_name);
}

static int f2(SCANDIR_ARG3 e)
{
        /*
         * We could maybe fallback to using lstat() here if
         * _DIRENT_HAVE_D_TYPE is not defined. But it is probably
         * way to slow to use in filters. That would also require
         * calls to getcwd() etc. since the current path is not
         * known in this context.
         */
#ifdef _DIRENT_HAVE_D_TYPE
        if (e->d_type != DT_UNKNOWN)
                return IS_RXX(e->d_name) && e->d_type == DT_REG;
#endif
        return IS_RXX(e->d_name);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void syncdir_scan(const char *dir, const char *root)
{
        struct dirent **namelist;
        unsigned int f;
        int (*filter[]) (SCANDIR_ARG3) = {f0, f1, f2};
#ifdef USE_RAR_PASSWORD
        char tmpbuf[MAXPASSWORD];
#endif
        const char *password = NULL;

        ENTER_("%s", dir);

        /* skip first filter; not needed */
        for (f = 1; f < (sizeof(filter) / sizeof(filter[0])); f++) {
                int i = 0;
                int n = scandir(root, &namelist, filter[f], alphasort);
                if (n < 0) {
                        perror("scandir");
                        return;
                }
                while (i < n) {
                        int vno = get_vformat(namelist[i]->d_name, !(f - 1),
                                                        NULL, NULL);
                        if (!OPT_INT(OPT_KEY_SEEK_LENGTH, 0) ||
                                        vno <= OPT_INT(OPT_KEY_SEEK_LENGTH, 0)) {
                                char *arch;
                                ABS_MP(arch, root, namelist[i]->d_name);
                                if (vno == 1 || (vno == 2 && f == 2)) /* "first" file */
                                        password = get_password(arch, tmpbuf);
                                (void)listrar(dir, NULL, arch, password);
                        }
                        free(namelist[i]);
                        ++i;
                }
                free(namelist);
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline int convert_fake_iso(char *name)
{
        size_t len;

        if (OPT_SET(OPT_KEY_FAKE_ISO)) {
                int l = OPT_CNT(OPT_KEY_FAKE_ISO)
                        ? optdb_find(OPT_KEY_FAKE_ISO, name)
                        : optdb_find(OPT_KEY_IMG_TYPE, name);
                if (!l)
                        return 0;
                len = strlen(name);
                if (l < 3)
                        name = realloc(name, len + 1 + (3 - l));
                strcpy(name + (len - l), "iso");
                return 1;
        }
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void readdir_scan(const char *dir, const char *root,
                struct dir_entry_list **next)
{
        struct dirent **namelist;
        unsigned int f;
        int (*filter[]) (SCANDIR_ARG3) = {f0, f1, f2};
#ifdef USE_RAR_PASSWORD
        char tmpbuf[MAXPASSWORD];
#endif
        const char *password = NULL;

        ENTER_("%s", dir);

        for (f = 0; f < (sizeof(filter) / sizeof(filter[0])); f++) {
                int i = 0;
                int n = scandir(root, &namelist, filter[f], alphasort);
                if (n < 0) {
                        perror("scandir");
                        continue;
                }
                while (i < n) {
                        if (!f) {
                                char *tmp = namelist[i]->d_name;
                                char *tmp2 = NULL;

                                /* Hide mount point in case of a fs loop */
                                if (fs_loop) {
                                        char *path;
                                        ABS_MP(path, root, tmp);
                                        if (!strcmp(path, OPT_STR2(OPT_KEY_DST, 0)))
                                                goto next_entry;
                                }
#ifdef _DIRENT_HAVE_D_TYPE
                                if (namelist[i]->d_type == DT_REG) {
#else
                                char *path;
                                struct stat st;
                                ABS_MP(path, root, tmp);
                                (void)stat(path, &st);
                                if (S_ISREG(st.st_mode)) {
#endif
                                        tmp2 = strdup(tmp);
                                        if (convert_fake_iso(tmp2))
                                                *next = dir_entry_add(*next, tmp2, NULL, DIR_E_NRM);
                                }
                                *next = dir_entry_add(*next, tmp, NULL, DIR_E_NRM);
                                if (tmp2 != NULL)
                                        free(tmp2);
                                goto next_entry;
                        }

                        int vno = get_vformat(namelist[i]->d_name, !(f - 1),
                                                        NULL, NULL);
                        if (!OPT_INT(OPT_KEY_SEEK_LENGTH, 0) ||
                                        vno <= OPT_INT(OPT_KEY_SEEK_LENGTH, 0)) {
                                char *arch;
                                ABS_MP(arch, root, namelist[i]->d_name);
                                if (vno == 1 || (vno == 2 && f == 2))   /* "first" file */
                                        password = get_password(arch, tmpbuf);
                                if (listrar(dir, next, arch, password))
                                        *next = dir_entry_add(*next, namelist[i]->d_name, NULL, DIR_E_RAR);
                        }

next_entry:

                        free(namelist[i]);
                        ++i;
                }
                free(namelist);
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void syncdir(const char *dir)
{
        ENTER_("%s", dir);

        DIR *dp;
        char *root;
        ABS_ROOT(root, dir);

        dp = opendir(root);
        if (dp != NULL) {
                syncdir_scan(dir, root);
                (void)closedir(dp);
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void syncrar(const char *path)
{
        ENTER_("%s", path);

#ifdef USE_RAR_PASSWORD
        char tmpbuf[MAXPASSWORD];
#endif
        const char *password = NULL;

        int c = 0;
        int c_end = OPT_INT(OPT_KEY_SEEK_LENGTH, 0);
        struct dir_entry_list *arch_next = arch_list_root.next;
        while (arch_next) {
                if (!c++)
                        password = get_password(arch_next->entry.name, tmpbuf);
                (void)listrar(path, NULL, arch_next->entry.name, password);
                if (c == c_end)
                        break;
                arch_next = arch_next->next;
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_getattr(const char *path, struct stat *stbuf)
{
        ENTER_("%s", path);
        pthread_mutex_lock(&file_access_mutex);
        if (cache_path(path, stbuf)) {
                pthread_mutex_unlock(&file_access_mutex);
                dump_stat(stbuf);
                return 0;
        }
        pthread_mutex_unlock(&file_access_mutex);

        /*
         * There was a cache miss and the file could not be found locally!
         * This is bad! To make sure the files does not really exist all
         * rar archives need to be scanned for a matching file = slow!
         */
        if (OPT_FILTER(path))
                return -ENOENT;
        char *safe_path = strdup(path);
        syncdir(dirname(safe_path));
        free(safe_path);
        pthread_mutex_lock(&file_access_mutex);
        dir_elem_t *e_p = cache_path_get(path);
        if (e_p) {
                memcpy(stbuf, &e_p->stat, sizeof(struct stat));
                pthread_mutex_unlock(&file_access_mutex);
                dump_stat(stbuf);
                return 0;
        }
        pthread_mutex_unlock(&file_access_mutex);
        return -ENOENT;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_getattr2(const char *path, struct stat *stbuf)
{
        ENTER_("%s", path);

        pthread_mutex_lock(&file_access_mutex);
        if (cache_path(path, stbuf)) {
                pthread_mutex_unlock(&file_access_mutex);
                dump_stat(stbuf);
                return 0;
        }
        pthread_mutex_unlock(&file_access_mutex);

        /*
         * There was a cache miss! To make sure the file does not really
         * exist the rar archive needs to be scanned for a matching file.
         * This should not happen very frequently unless the contents of
         * the rar archive was actually changed after it was mounted.
         */
        syncrar("/");

        pthread_mutex_lock(&file_access_mutex);
        dir_elem_t *e_p = cache_path_get(path);
        if (e_p) {
                memcpy(stbuf, &e_p->stat, sizeof(struct stat));
                pthread_mutex_unlock(&file_access_mutex);
                dump_stat(stbuf);
                return 0;
        }
        pthread_mutex_unlock(&file_access_mutex);
        return -ENOENT;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void dump_dir_list(const char *path, void *buffer, fuse_fill_dir_t filler,
                struct dir_entry_list *next)
{
        ENTER_();

        int do_inval_cache = 1;

        next = next->next;
        while (next) {
                /*
                 * Purge invalid entries from the cache, display the rest.
                 * Collisions are rare but might occur when a file inside
                 * a RAR archives share the same name with a file in the
                 * back-end fs. The latter will prevail in all cases.
                 */
                if (next->entry.valid) {
                        filler(buffer, next->entry.name, next->entry.st, 0);
                        if (next->entry.type == DIR_E_RAR)
                                do_inval_cache = 0;
                        else
                                do_inval_cache = 1;
                } else {
                        if (do_inval_cache ||
                            next->entry.type == DIR_E_NRM) { /* Oops! */
                                char *tmp;
                                ABS_MP(tmp, path, next->entry.name);
                                inval_cache_path(tmp);
                        }
                }
                next = next->next;
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
                off_t offset, struct fuse_file_info *fi)
{
        ENTER_("%s", path);

        (void)offset;           /* touch */
        (void)fi;               /* touch */

#ifdef USE_RAR_PASSWORD
        char tmpbuf[MAXPASSWORD];
#endif
        const char *password = NULL;
        struct dir_entry_list dir_list;      /* internal list root */
        struct dir_entry_list *next = &dir_list;
        dir_list_open(next);

        DIR *dp;
        char *root;
        ABS_ROOT(root, path);

        dp = opendir(root);
        if (dp != NULL) {
                readdir_scan(path, root, &next);
                (void)closedir(dp);
                goto dump_buff;
        }

        int vol = 1;

        pthread_mutex_lock(&file_access_mutex);
        dir_elem_t *entry_p = cache_path_get(path);
        if (entry_p) {
                char *tmp = strdup(entry_p->rar_p);
                int multipart = entry_p->flags.multipart;
                short vtype = entry_p->vtype;
                pthread_mutex_unlock(&file_access_mutex);
                if (multipart) {
                        do {
                                printd(3, "Search for local directory in %s\n", tmp);
                                if (vol++ == 1) { /* first file */
                                        password = get_password(tmp, tmpbuf);
                                } else {
                                        RARNextVolumeName(tmp, !vtype);
                                }
                        } while (!listrar(path, &next, tmp, password));
                } else { 
                        if (tmp) {
                                printd(3, "Search for local directory in %s\n", tmp);
                                password = get_password(tmp, tmpbuf);
                                if (!listrar(path, &next, tmp, password)) {
                                        free(tmp);
                                        goto fill_buff;
                                }
                        }
                }
                free(tmp);
        } else {
                pthread_mutex_unlock(&file_access_mutex);
        }

        if (vol < 3) {  /* First attempt failed! */
                dir_list_free(&dir_list);
                /*
                 * Fuse bug!? Returning -ENOENT here seems to be
                 * silently ignored. Typically errno is here set to
                 * ESPIPE (aka "Illegal seek").
                 */
                return -errno;
        }

fill_buff:

        filler(buffer, ".", NULL, 0);
        filler(buffer, "..", NULL, 0);

dump_buff:

        dir_list_close(&dir_list);
        dump_dir_list(path, buffer, filler, &dir_list);
        dir_list_free(&dir_list);

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_readdir2(const char *path, void *buffer,
                fuse_fill_dir_t filler, off_t offset,
                struct fuse_file_info *fi)
{
        ENTER_("%s", path);

        (void)offset;           /* touch */
        (void)fi;               /* touch */

#ifdef USE_RAR_PASSWORD
        char tmpbuf[MAXPASSWORD];
#endif
        const char *password = NULL;
        struct dir_entry_list dir_list;      /* internal list root */
        struct dir_entry_list *next = &dir_list;
        dir_list_open(next);

        int c = 0;
        int c_end = OPT_INT(OPT_KEY_SEEK_LENGTH, 0);
        struct dir_entry_list *arch_next = arch_list_root.next;
        while (arch_next) {
                if (!c++)
                        password = get_password(arch_next->entry.name, tmpbuf);
                (void)listrar(path, &next, arch_next->entry.name, password);
                if (c == c_end)
                        break;
                arch_next = arch_next->next;
        }

        filler(buffer, ".", NULL, 0);
        filler(buffer, "..", NULL, 0);

        dir_list_close(&dir_list);
        dump_dir_list(path, buffer, filler, &dir_list);
        dir_list_free(&dir_list);

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *reader_task(void *arg)
{
        struct io_context *op = (struct io_context *)arg;
        op->terminate = 0;

        printd(4, "Reader thread started, fp=%p\n", op->fp);

        int fd = op->pfd1[0];
        int nfsd = fd + 1;
        while (!op->terminate) {
                fd_set rd;
                struct timeval tv;
                tv.tv_sec = 1;
                tv.tv_usec = 0;
                FD_ZERO(&rd);
                FD_SET(fd, &rd);
                int retval = select(nfsd, &rd, NULL, NULL, &tv);
                if (!retval) {
                        /* timeout */
                        if (fs_terminated) {
                                if (!pthread_mutex_trylock(&op->mutex)) {
                                        op->terminate = 1;
                                        pthread_mutex_unlock(&op->mutex);
                                }
                        }
                        continue;
                }
                if (retval == -1) {
                        perror("select()");
                        continue;
                }
                /* FD_ISSET(0, &rfds) will be true. */
                printd(4, "Reader thread wakeup, select()=%d\n", retval);
                char buf[2];
                NO_UNUSED_RESULT read(fd, buf, 1);      /* consume byte */
                if (buf[0] < 2 /*&& !feof(op->fp)*/)
                        (void)readTo(op->buf, op->fp, IOB_SAVE_HIST);
                if (buf[0]) {
                        printd(4, "Reader thread acknowledge\n");
                        int fd = op->pfd2[1];
                        if (write(fd, buf, 1) != 1)
                                perror("write");
                }
#if 0
                /* Early termination */
                if (feof(op->fp)) {
                        if (!pthread_mutex_trylock(&op->mutex)) {
                                op->terminate = 1;
                                pthread_mutex_unlock(&op->mutex);
                        }
                }
#endif
        }
        printd(4, "Reader thread stopped\n");
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int preload_index(struct io_buf *buf, const char *path)
{
        ENTER_("%s", path);

        /* check for .avi or .mkv */
        if (!IS_AVI(path) && !IS_MKV(path))
                return -1;

        char *r2i;
        ABS_ROOT(r2i, path);
        strcpy(&r2i[strlen(r2i) - 3], "r2i");
        printd(3, "Preloading index for %s\n", r2i);

        buf->idx.data_p = MAP_FAILED;
        int fd = open(r2i, O_RDONLY);
        if (fd == -1) {
                return -1;
        }

#ifdef HAVE_MMAP
        /* Map the file into address space (1st pass) */
        struct idx_head *h = (struct idx_head *)mmap(NULL, 
                        sizeof(struct idx_head), PROT_READ, MAP_SHARED, fd, 0);
        if (h == MAP_FAILED || h->magic != R2I_MAGIC) {
                close(fd);
                return -1;
        }

        /* Map the file into address space (2nd pass) */
        buf->idx.data_p = (void *)mmap(NULL, P_ALIGN_(h->size), PROT_READ,
                                                 MAP_SHARED, fd, 0);
        munmap((void *)h, sizeof(struct idx_head));
        if (buf->idx.data_p == MAP_FAILED) {
                close(fd);
                return -1;
        }
        buf->idx.mmap = 1;
#else
        buf->idx.data_p = malloc(sizeof(struct idx_data));
        if (!buf->idx.data_p) {
                buf->idx.data_p = MAP_FAILED;
                return -1;
        }
        NO_UNUSED_RESULT read(fd, buf->idx.data_p, sizeof(struct idx_head));
        buf->idx.mmap = 0;
#endif
        buf->idx.fd = fd;
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline int pow_(int b, int n)
{
        int p = 1;
        while (n--)
                p *= b;
        return p;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/

#define LE_BYTES_TO_W32(b) \
        (uint32_t)(*((b)+3) * 16777216 + *((b)+2) * 65537 + *((b)+1) * 256 + *(b))

static int check_avi_type(struct io_context *op)
{
        uint32_t off = 0;
        uint32_t off_end = 0;
        uint32_t len = 0;
        uint32_t first_fc = 0;

        sleep(1);
        if (!(op->buf->data_p[off + 0] == 'R' &&
              op->buf->data_p[off + 1] == 'I' &&
              op->buf->data_p[off + 2] == 'F' &&
              op->buf->data_p[off + 3] == 'F')) {
                return -1;
        }
        off += 8;
        if (!(op->buf->data_p[off + 0] == 'A' &&
              op->buf->data_p[off + 1] == 'V' &&
              op->buf->data_p[off + 2] == 'I' &&
              op->buf->data_p[off + 3] == ' ')) {
                return -1;
        }
        off += 4;
        if (!(op->buf->data_p[off + 0] == 'L' &&
              op->buf->data_p[off + 1] == 'I' &&
              op->buf->data_p[off + 2] == 'S' &&
              op->buf->data_p[off + 3] == 'T')) {
                return -1;
        }
        off += 4;
        len = LE_BYTES_TO_W32(op->buf->data_p + off);

        /* Search ends here */
        off_end = len + 20;

        /* Locate the AVI header and extract frame count. */
        off += 8;
        if (!(op->buf->data_p[off + 0] == 'a' &&
              op->buf->data_p[off + 1] == 'v' &&
              op->buf->data_p[off + 2] == 'i' &&
              op->buf->data_p[off + 3] == 'h')) {
                return -1;
        }
        off += 4;
        len = LE_BYTES_TO_W32(op->buf->data_p + off);

        /* The frame count will be compared with a possible multi-part
         * OpenDML (AVI 2.0) to detect a badly configured muxer. */
        off += 4;
        first_fc = LE_BYTES_TO_W32(op->buf->data_p + off + 16);

        off += len;
        for (; off < off_end; off += len) {
                off += 4;
                len = LE_BYTES_TO_W32(op->buf->data_p + off);
                off += 4;
                if (op->buf->data_p[off + 0] == 'o' &&
                    op->buf->data_p[off + 1] == 'd' &&
                    op->buf->data_p[off + 2] == 'm' &&
                    op->buf->data_p[off + 3] == 'l' &&
                    op->buf->data_p[off + 4] == 'd' &&
                    op->buf->data_p[off + 5] == 'm' &&
                    op->buf->data_p[off + 6] == 'l' &&
                    op->buf->data_p[off + 7] == 'h') {
                        off += 12;
                        /* Check AVI 2.0 frame count */
                        if (first_fc != LE_BYTES_TO_W32(op->buf->data_p + off))
                                return -1;
                        return 0;
                }
        }

        return 0;
}

#undef LE_BYTE_TO_W32

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_open(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", path);

        printd(3, "(%05d) %-8s%s [0x%llu][called from %05d]\n", getpid(),
                        "OPEN", path, fi->fh, fuse_get_context()->pid);
        dir_elem_t *entry_p;
        char *root;

        errno = 0;
        pthread_mutex_lock(&file_access_mutex);
        entry_p = cache_path(path, NULL);

        if (entry_p == NULL) {
                pthread_mutex_unlock(&file_access_mutex);
                return -ENOENT;
        }
        if (entry_p == LOCAL_FS_ENTRY) {
                /* In case of O_TRUNC it will simply be passed to open() */
                pthread_mutex_unlock(&file_access_mutex);
                ABS_ROOT(root, path);
                return lopen(root, fi);
        }
        if (entry_p->flags.fake_iso) {
                int res;
                dir_elem_t *e_p = filecache_clone(entry_p);
                pthread_mutex_unlock(&file_access_mutex);
                if (e_p) {
                        ABS_ROOT(root, e_p->file_p);
                        res = lopen(root, fi);
                        if (res == 0) {
                                /* Override defaults */
                                FH_SETTYPE(fi->fh, IO_TYPE_ISO);
                                FH_SETENTRY(fi->fh, e_p);
                        } else {
                                filecache_freeclone(e_p);
                        }
                } else {
                        res = -EIO;
                }
                return res; 
        }
        /*
         * For files inside RAR archives open for exclusive write access
         * is not permitted. That implicity includes also O_TRUNC.
         * O_CREAT/O_EXCL is never passed to open() by FUSE so no need to
         * check those.
         */
        if (fi->flags & (O_WRONLY | O_TRUNC)) {
                pthread_mutex_unlock(&file_access_mutex);
                return -EPERM;
        }

        FILE *fp = NULL;
        struct io_buf *buf = NULL;
        struct io_context *op = NULL;
        struct io_handle* io = NULL;
        pid_t pid = 0;

        if (!FH_ISSET(fi->fh)) {
                if (entry_p->flags.raw) {
                        FILE *fp = fopen(entry_p->rar_p, "r");
                        if (fp != NULL) {
                                io = malloc(sizeof(struct io_handle));
                                op = malloc(sizeof(struct io_context));
                                if (!op || !io)
                                        goto open_error;
                                printd(3, "Opened %s\n", entry_p->rar_p);
                                FH_SETIO(fi->fh, io);
                                FH_SETTYPE(fi->fh, IO_TYPE_RAW);
                                FH_SETENTRY(fi->fh, NULL);
                                FH_SETCONTEXT(fi->fh, op);
                                printd(3, "(%05d) %-8s%s [%-16p]\n", getpid(), "ALLOC", path, FH_TOCONTEXT(fi->fh));
                                op->fp = fp;
                                op->pid = 0;
                                op->seq = 0;
                                op->buf = NULL;
                                op->entry_p = NULL;
                                op->pos = 0;
                                op->vno = -1;   /* force a miss 1:st time */
                                if (entry_p->flags.multipart &&
                                                OPT_SET(OPT_KEY_PREOPEN_IMG) &&
                                                entry_p->flags.image) {
                                        if (entry_p->vtype == 1) {  /* New numbering */
                                                entry_p->vno_max =
                                                    pow_(10, entry_p->vlen) - 1;
                                        } else {
                                                 /* 
                                                  * Old numbering is more than obscure when
                                                  * it comes to maximum value. Lets assume 
                                                  * something high (almost realistic) here.
                                                  * Will probably hit the open file limit 
                                                  * anyway.
                                                  */
                                                 entry_p->vno_max = 901;  /* .rar -> .z99 */
                                        }
                                        op->volHdl =
                                            malloc(entry_p->vno_max * sizeof(struct vol_handle));
                                        if (op->volHdl) {
                                                memset(op->volHdl, 0, entry_p->vno_max * sizeof(struct vol_handle));
                                                char *tmp = strdup(entry_p->rar_p);
                                                int j = entry_p->vno_max;
                                                while (j--) {
                                                        FILE *fp_ = fopen(tmp, "r");
                                                        if (fp_ == NULL) {
                                                                break;
                                                        }
                                                        printd(3, "pre-open %s\n", tmp);
                                                        op->volHdl[j].fp = fp_;
                                                        op->volHdl[j].pos = VOL_REAL_SZ - VOL_FIRST_SZ;
                                                        printd(3, "SEEK src_off = %llu\n", op->volHdl[j].pos);
                                                        fseeko(fp_, op->volHdl[j].pos, SEEK_SET);
                                                        RARNextVolumeName(tmp, !entry_p->vtype);
                                                }
                                                free(tmp);
                                        } else {
                                                printd(1, "Failed to allocate resource (%u)\n", __LINE__);
                                        }
                                } else {
                                        op->volHdl = NULL;
                                }

                                /*
                                 * Make sure cache entry is filled in completely
                                 * before cloning it
                                 */
                                op->entry_p = filecache_clone(entry_p);
                                if (!op->entry_p) 
                                        goto open_error;
                                goto open_end;
                        }

                        goto open_error;
                }

                void *mmap_addr = NULL;
                FILE *mmap_fp = NULL;
                int mmap_fd = 0;

                buf = malloc(P_ALIGN_(sizeof(struct io_buf) + IOB_SZ));
                if (!buf)
                        goto open_error;
                IOB_RST(buf);

                io = malloc(sizeof(struct io_handle));
                op = malloc(sizeof(struct io_context));
                if (!op || !io)
                        goto open_error;
                op->buf = buf;
                op->entry_p = NULL;

                /* Open PIPE(s) and create child process */
                fp = popen_(entry_p, &pid, &mmap_addr, &mmap_fp, &mmap_fd);
                if (fp != NULL) {
                        FH_SETIO(fi->fh, io);
                        FH_SETTYPE(fi->fh, IO_TYPE_RAR);
                        FH_SETENTRY(fi->fh, NULL);
                        FH_SETCONTEXT(fi->fh, op);
                        printd(3, "(%05d) %-8s%s [%-16p]\n", getpid(), "ALLOC",
                                                path, FH_TOCONTEXT(fi->fh));
                        op->seq = 0;
                        op->pos = 0;
                        op->fp = fp;
                        op->pid = pid;
                        printd(4, "PIPE %p created towards child %d\n",
                                                op->fp, pid);

                        /*
                         * Create pipes to be used between threads.
                         * Both these pipes are used for communication between
                         * parent (this thread) and reader thread. One pipe is for
                         * requests (w->r) and the other is for responses (r<-w).
                         */
                        op->pfd1[0] = -1;
                        op->pfd1[1] = -1;
                        op->pfd2[0] = -1;
                        op->pfd2[1] = -1;
                        if (pipe(op->pfd1) == -1) {
                                perror("pipe");
                                goto open_error;
                        }
                        if (pipe(op->pfd2) == -1) {
                                perror("pipe");
                                goto open_error;
                        }

                        pthread_mutex_init(&op->mutex, NULL);

                        /*
                         * Disable flushing the cache of the file contents on every open().
                         * This is important to make sure FUSE does not force read from an
                         * old offset. That could break the logic for compressed/encrypted
                         * archives since the I/O context will become out-of-sync.
                         * This should only be enabled on files, where the file data is never
                         * changed externally (not through the mounted FUSE filesystem).
                         * But first see 'direct_io' below.
                         */
                        fi->keep_cache = 1;

                        /*
                         * The below will take precedence over keep_cache.
                         * This flag will allow the filesystem to bypass the page cache using
                         * the "direct_io" flag.  This is not the same as O_DIRECT, it's
                         * dictated by the filesystem not the application.
                         * Since compressed archives might sometimes require fake data to be
                         * returned in read requests, a cache might cause the same faulty
                         * information to be propagated to sub-sequent reads. Setting this
                         * flag will force _all_ reads to enter the filesystem.
                         */
                        if (entry_p->flags.direct_io)
                                fi->direct_io = 1;

                        /* Create reader thread */
                        op->terminate = 1;
                        pthread_create(&op->thread, &thread_attr, reader_task, (void *)op);
                        while (op->terminate);
                        WAKE_THREAD(op->pfd1, 0);

                        buf->idx.data_p = MAP_FAILED;
                        buf->idx.fd = -1;
                        if (!preload_index(buf, path)) {
                                entry_p->flags.save_eof = 0;
                                entry_p->flags.direct_io = 0;
                                fi->direct_io = 0;
                        } else {
                                /* Was the file removed ? */
                                if (OPT_SET(OPT_KEY_SAVE_EOF) && !entry_p->flags.save_eof) {
                                        if (!entry_p->password_p) {
                                                entry_p->flags.save_eof = 1;
                                                entry_p->flags.avi_tested = 0;
                                        }
                                }
                        }
                        op->mmap_addr = mmap_addr;
                        op->mmap_fp = mmap_fp;
                        op->mmap_fd = mmap_fd;

                        if (entry_p->flags.save_eof && !entry_p->flags.avi_tested) {
                                if (check_avi_type(op))
                                        entry_p->flags.save_eof = 0;
                                entry_p->flags.avi_tested = 1;
                        }

#ifdef DEBUG_READ
                        char out_file[32];
                        sprintf(out_file, "%s.%d", "output", pid);
                        op->dbg_fp = fopen(out_file, "w");
#endif
                        /*
                         * Make sure cache entry is filled in completely
                         * before cloning it
                         */
                        op->entry_p = filecache_clone(entry_p);
                        if (!op->entry_p) 
                                goto open_error;
                        goto open_end;
                }

                goto open_error;
        }

open_error:
        pthread_mutex_unlock(&file_access_mutex);
        if (fp)
                pclose_(fp, pid);
        if (op) {
                if (op->pfd1[0] >= 0)
                        close(op->pfd1[0]);
                if (op->pfd1[1] >= 0)
                        close(op->pfd1[1]);
                if (op->pfd2[0] >= 0)
                        close(op->pfd2[0]);
                if (op->pfd2[1] >= 0)
                        close(op->pfd2[1]);
                if (op->entry_p)
                        filecache_freeclone(op->entry_p);
                free(op);
        }
        if (buf)
                free(buf);

        /*
         * This is the best we can return here. So many different things
         * might go wrong and errno can actually be set to something that
         * FUSE is accepting and thus proceeds with next operation!
         */
        printd(1, "open: I/O error\n");
        return -EIO;

open_end:
        pthread_mutex_unlock(&file_access_mutex);
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline int access_chk(const char *path, int new_file)
{
        void *e;

        /*
         * To return a more correct fault code if an attempt is
         * made to create/remove a file in a RAR folder, a cache lookup
         * will tell if operation should be permitted or not.
         * Simply, if the file/folder is in the cache, forget it!
         *   This works fine in most cases but it does not work for some
         * specific programs like 'touch'. A 'touch' may result in a
         * getattr() callback even if -EPERM is returned by open() which
         * will eventually render a "No such file or directory" type of
         * error/message.
         */
        if (new_file) {
                char *p = strdup(path); /* In case p is destroyed by dirname() */
                e = (void *)cache_path_get(dirname(p));
                free(p);
        } else {
                e = (void *)cache_path_get(path);
        }
        return e ? 1 : 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *rar2_init(struct fuse_conn_info *conn)
{
        ENTER_();

        (void)conn;             /* touch */

        filecache_init();
        iobuffer_init();
        sighandler_init();

#ifdef HAVE_FMEMOPEN
        /* Check fmemopen() support */
        {
                char tmp[64];
                glibc_test = 1;
                fclose(fmemopen(tmp, 64, "r"));
                glibc_test = 0;
        }
#endif

        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void rar2_destroy(void *data)
{
        ENTER_();

        (void)data;             /* touch */

        iobuffer_destroy();
        filecache_destroy();
        sighandler_destroy();
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_flush(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", path);
        printd(3, "(%05d) %-8s%s [%-16p][called from %05d]\n", getpid(),
               "FLUSH", path, FH_TOCONTEXT(fi->fh), fuse_get_context()->pid);
        return lflush(path, fi);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_readlink(const char *path, char *buf, size_t buflen)
{
        ENTER_("%s", path);
        char *tmp;
        ABS_ROOT(tmp, path);
        buflen = readlink(tmp, buf, buflen);
        if ((ssize_t)buflen >= 0) {
                buf[buflen] = 0;
                return 0;
        }
        return -errno;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_statfs(const char *path, struct statvfs *vfs)
{
        ENTER_("%s", path);
        (void)path;             /* touch */
        if (!statvfs(OPT_STR2(OPT_KEY_SRC, 0), vfs))
                return 0;
        return -errno;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_release(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", path);
        printd(3, "(%05d) %-8s%s [%-16p]\n", getpid(), "RELEASE", path,
               FH_TOCONTEXT(fi->fh));
        if (!FH_ISSET(fi->fh)) {
                pthread_mutex_lock(&file_access_mutex);
                inval_cache_path(path);
                pthread_mutex_unlock(&file_access_mutex);
                return 0;
        }

        if (FH_TOIO(fi->fh)->type == IO_TYPE_RAR ||
                        FH_TOIO(fi->fh)->type == IO_TYPE_RAW) {
                struct io_context *op = FH_TOCONTEXT(fi->fh);
                if (op->fp) {
                        if (op->entry_p->flags.raw) {
                                if (op->volHdl) {
                                        int j;
                                        for (j = 0; j < op->entry_p->vno_max; j++) {
                                                if (op->volHdl[j].fp)
                                                        fclose(op->volHdl[j].fp);
                                        }
                                        free(op->volHdl);
                                }
                                fclose(op->fp);
                                printd(3, "Closing file handle %p\n", op->fp);
                        } else {
                                if (!op->terminate) {
                                        op->terminate = 1;
                                        WAKE_THREAD(op->pfd1, 0);
                                }
                                pthread_join(op->thread, NULL);
                                if (pclose_(op->fp, op->pid)) {
                                        printd(4, "child closed abnormaly");
                                }
                                printd(4, "PIPE %p closed towards child %05d\n",
                                               op->fp, op->pid);

                                close(op->pfd1[0]);
                                close(op->pfd1[1]);
                                close(op->pfd2[0]);
                                close(op->pfd2[1]);

#ifdef DEBUG_READ
                                fclose(op->dbg_fp);
#endif

                                if (op->entry_p->flags.mmap) {
                                        fclose(op->mmap_fp);
                                        if (op->mmap_addr != MAP_FAILED) {
                                                if (op->entry_p->flags.mmap == 1)
                                                        munmap(op->mmap_addr, P_ALIGN_(op->entry_p->msize));
                                                else
                                                        free(op->mmap_addr);
                                        }
                                        close(op->mmap_fd);
                                }
                                pthread_mutex_destroy(&op->mutex);
                        }
                }
                printd(3, "(%05d) %-8s%s [%-16p]\n", getpid(), "FREE", path, op);
                if (op->buf) {
                        /* XXX clean up */
#ifdef HAVE_MMAP
                        if (op->buf->idx.data_p != MAP_FAILED && 
                                        op->buf->idx.mmap)
                                munmap((void *)op->buf->idx.data_p, 
                                                P_ALIGN_(op->buf->idx.data_p->head.size));
#endif
                        if (op->buf->idx.data_p != MAP_FAILED &&
                                        !op->buf->idx.mmap)
                                free(op->buf->idx.data_p);
                        if (op->buf->idx.fd != -1)
                                close(op->buf->idx.fd);
                        free(op->buf);
                }
                filecache_freeclone(op->entry_p);
                free(op);
                free(FH_TOIO(fi->fh));
                FH_ZERO(fi->fh);
        } else {
                char *root;
                ABS_ROOT(root, path);
                return lrelease(root, fi);
        }

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_read(const char *path, char *buffer, size_t size, off_t offset,
                struct fuse_file_info *fi)
{
        int res;
        struct io_handle *io;

        assert(FH_ISSET(fi->fh) && "bad I/O handle");

        io = FH_TOIO(fi->fh);
        if (!io)
               return -EIO;
#if 0
        int direct_io = fi->direct_io;
#endif

        ENTER_("%s   size=%zu, offset=%llu, fh=%llu", path, size, offset, fi->fh);

        if (io->type == IO_TYPE_NRM) {
                char *root;
                ABS_ROOT(root, path);
                res = lread(root, buffer, size, offset, fi);
        } else if (io->type == IO_TYPE_ISO) {
                char *root;
                ABS_ROOT(root, io->entry_p->file_p);
                res = lread(root, buffer, size, offset, fi);
        } else if (io->type == IO_TYPE_RAW) {
                res = lread_raw(buffer, size, offset, fi);
        } else {
                res = lread_rar(buffer, size, offset, fi);
        }

#if 0
        if (res < 0 && direct_io) {
                errno = -res;   /* convert to proper errno? */
                return -1;
        }
#endif
        return res;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_truncate(const char *path, off_t offset)
{
        ENTER_("%s", path);
        if (!access_chk(path, 0)) {
                char *root;
                ABS_ROOT(root, path);
                if (!truncate(root, offset))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_write(const char *path, const char *buffer, size_t size,
                off_t offset, struct fuse_file_info *fi)
{
        char *root;
        ssize_t n;

        ENTER_("%s", path);

        ABS_ROOT(root, path);
        n = pwrite(FH_TOFD(fi->fh), buffer, size, offset);
        return n >= 0 ? n : -errno;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_chmod(const char *path, mode_t mode)
{
        ENTER_("%s", path);
        if (!access_chk(path, 0)) {
                char *root;
                ABS_ROOT(root, path);
                if (!chmod(root, mode))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_chown(const char *path, uid_t uid, gid_t gid)
{
        ENTER_("%s", path);
        if (!access_chk(path, 0)) {
                char *root;
                ABS_ROOT(root, path);
                if (!chown(root, uid, gid))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
        ENTER_("%s", path);
        /* Only allow creation of "regular" files this way */
        if (S_ISREG(mode)) {
                if (!access_chk(path, 1)) {
                        char *root;
                        ABS_ROOT(root, path);
                        int fd = creat(root, mode);
                        if (fd == -1)
                                return -errno;
                        if (!FH_ISSET(fi->fh)) {
                                struct io_handle *io =
                                        malloc(sizeof(struct io_handle));
                                /* 
                                 * Does not really matter what is returned in
                                 * case of failure as long as it is not 0.
                                 * Returning anything but 0 will avoid the
                                 * _release() call but will still create the
                                 * file when called from e.g. 'touch'.
                                 */
                                if (!io) {
                                        close(fd);
                                        return -EIO;
                                }
                                FH_SETIO(fi->fh, io);
                                FH_SETTYPE(fi->fh, IO_TYPE_NRM);
                                FH_SETENTRY(fi->fh, NULL);
                                FH_SETFD(fi->fh, fd);
                        }
                        return 0;
                }
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_eperm_stub()
{
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_rename(const char *oldpath, const char *newpath)
{
        ENTER_("%s", oldpath);
        /* We can not move things out of- or from RAR archives */
        if (!access_chk(newpath, 1) && !access_chk(oldpath, 0)) {
                char *oldroot;
                char *newroot;
                ABS_ROOT(oldroot, oldpath);
                ABS_ROOT(newroot, newpath);
                if (!rename(oldroot, newroot))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_mknod(const char *path, mode_t mode, dev_t dev)
{
        ENTER_("%s", path);
        if (!access_chk(path, 1)) {
                char *root;
                ABS_ROOT(root, path);
                if (!mknod(root, mode, dev))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_unlink(const char *path)
{
        ENTER_("%s", path);
        if (!access_chk(path, 0)) {
                char *root;
                ABS_ROOT(root, path);
                if (!unlink(root))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_mkdir(const char *path, mode_t mode)
{
        ENTER_("%s", path);
        if (!access_chk(path, 1)) {
                char *root;
                ABS_ROOT(root, path);
                if (!mkdir(root, mode))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_rmdir(const char *path)
{
        ENTER_("%s", path);
        if (!access_chk(path, 0)) {
                char *root;
                ABS_ROOT(root, path);
                if (!rmdir(root))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_utimens(const char *path, const struct timespec ts[2])
{
        ENTER_("%s", path);

        if (!access_chk(path, 0)) {
                int res;
                struct timeval tv[2];
                char *root;
                ABS_ROOT(root, path);

                tv[0].tv_sec = ts[0].tv_sec;
                tv[0].tv_usec = ts[0].tv_nsec / 1000;
                tv[1].tv_sec = ts[1].tv_sec;
                tv[1].tv_usec = ts[1].tv_nsec / 1000;

                res = utimes(root, tv);
                if (res == -1)
                        return -errno;
                return 0;
        }
        return -EPERM;
}

#ifdef HAVE_SETXATTR

/*!
*****************************************************************************
*
****************************************************************************/
#ifdef XATTR_ADD_OPT
static int rar2_getxattr(const char *path, const char *name, char *value,
                size_t size, uint32_t position)
#else
static int rar2_getxattr(const char *path, const char *name, char *value,
                size_t size)
#endif
{
        ENTER_("%s", path);

        if (!access_chk(path, 0)) {
                char *tmp;
                ABS_ROOT(tmp, path);
#ifdef XATTR_ADD_OPT
                size = getxattr(tmp, name, value, size, position,
                                        XATTR_NOFOLLOW);
#else
                size = lgetxattr(tmp, name, value, size);
#endif
                if ((ssize_t)size == -1)
                        return -errno;
                return size;
        }
        return -ENOTSUP;
}

/*!
*****************************************************************************
*
****************************************************************************/
static int rar2_listxattr(const char *path, char *list, size_t size)
{
        ENTER_("%s", path);

        if (!access_chk(path, 0)) {
                char *tmp;
                ABS_ROOT(tmp, path);
#ifdef XATTR_ADD_OPT
                size = listxattr(tmp, list, size, XATTR_NOFOLLOW);
#else
                size = llistxattr(tmp, list, size);
#endif
                if ((ssize_t)size == -1)
                        return -errno;
                return size;
        }
        return -ENOTSUP;
}

/*!
*****************************************************************************
*
****************************************************************************/
#ifdef XATTR_ADD_OPT
static int rar2_setxattr(const char *path, const char *name, const char *value,
                size_t size, int flags, uint32_t position)
#else
static int rar2_setxattr(const char *path, const char *name, const char *value,
                size_t size, int flags)
#endif
{
        ENTER_("%s", path);

        if (!access_chk(path, 0)) {
                char *tmp;
                ABS_ROOT(tmp, path);
#ifdef XATTR_ADD_OPT
                size = setxattr(tmp, name, value, size, position,
                                        flags | XATTR_NOFOLLOW);
#else
                size = lsetxattr(tmp, name, value, size, flags);
#endif
                if ((ssize_t)size == -1)
                        return -errno;
                return 0;
        }
        return -ENOTSUP;
}

/*!
*****************************************************************************
*
****************************************************************************/
static int rar2_removexattr(const char *path, const char *name)
{
        ENTER_("%s", path);

        if (!access_chk(path, 0)) {
                int res;
                char *tmp;
                ABS_ROOT(tmp, path);
#ifdef XATTR_ADD_OPT
                res = removexattr(tmp, name, XATTR_NOFOLLOW);
#else
                res = lremovexattr(tmp, name);
#endif
                if (res == -1)
                        return -errno;
                return 0;
        }
        return -ENOTSUP;
}
#endif

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void usage(char *prog)
{
        const char *P_ = basename(prog);
        printf("Usage: %s source mountpoint [options]\n", P_);
        printf("Try `%s -h' or `%s --help' for more information.\n", P_, P_);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int check_paths(const char *prog, char *src_path_in, char *dst_path_in,
                char **src_path_out, char **dst_path_out, int verbose)
{
        char p1[PATH_MAX];
        char p2[PATH_MAX];
        char *a1 = realpath(src_path_in, p1);
        char *a2 = realpath(dst_path_in, p2);
        if (!a1 || !a2 || !strcmp(a1, a2)) {
                if (verbose)
                        printf("%s: invalid source and/or mount point\n",
                                                prog);
                return -1;
        }
        dir_list_open(arch_list);
        struct stat st;
        (void)stat(a1, &st);
        mount_type = S_ISDIR(st.st_mode) ? MOUNT_FOLDER : MOUNT_ARCHIVE;
        /* Check path type(s), destination path *must* be a folder */
        (void)stat(a2, &st);
        if (!S_ISDIR(st.st_mode) ||
                        (mount_type == MOUNT_ARCHIVE &&
                        !collect_files(a1, arch_list))) {
                if (verbose)
                        printf("%s: invalid source and/or mount point\n",
                                                prog);
                return -1;
        }
        /* Do not try to use 'a1' after this call since dirname() will destroy it! */
        *src_path_out = mount_type == MOUNT_FOLDER
                ? strdup(a1) : strdup(dirname(a1));
        *dst_path_out = strdup(a2);

        /* Detect a possible file system loop */
        if (mount_type == MOUNT_FOLDER) {
                if (!strncmp(*src_path_out, *dst_path_out,
                                        strlen(*src_path_out))) {
                        if ((*dst_path_out)[strlen(*src_path_out)] == '/')
                                fs_loop = 1;
                }
        }

        /* Do not close the list since it could re-order the entries! */
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int check_iob(char *bname, int verbose)
{
        unsigned int bsz = OPT_INT(OPT_KEY_BUF_SIZE, 0);
        unsigned int hsz = OPT_INT(OPT_KEY_HIST_SIZE, 0);
        if ((OPT_SET(OPT_KEY_BUF_SIZE) && !bsz) || (bsz & (bsz - 1)) ||
                        (OPT_SET(OPT_KEY_HIST_SIZE) && (hsz > 75))) {
                if (verbose)
                        usage(bname);
                return -1;
        }
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int check_libunrar(int verbose)
{
        if (RARGetDllVersion() != RAR_DLL_VERSION) {
                if (verbose) {
                        if (RARVER_BETA) {
                                printf("libunrar.so (v%d.%d beta %d) or compatible library not found\n",
                                       RARVER_MAJOR, RARVER_MINOR, RARVER_BETA);
                        } else {
                                printf("libunrar.so (v%d.%d) or compatible library not found\n",
                                       RARVER_MAJOR, RARVER_MINOR);
                        }
                }
                return -1;
        }
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int check_libfuse(int verbose)
{
        if (fuse_version() < FUSE_VERSION) {
                if (verbose)
                        printf("libfuse.so.%d.%d or compatible library not found\n",
                               FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION);
                return -1;
        }
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/

/* Mapping of static/non-configurable FUSE file system operations. */
static struct fuse_operations rar2_operations = {
        .init = rar2_init,
        .statfs = rar2_statfs,
        .utimens = rar2_utimens,
        .destroy = rar2_destroy,
        .open = rar2_open,
        .release = rar2_release,
        .read = rar2_read,
        .flush = rar2_flush,
        .readlink = rar2_readlink,
#ifdef HAVE_SETXATTR
        .getxattr = rar2_getxattr,
        .setxattr = rar2_setxattr,
        .listxattr = rar2_listxattr,
        .removexattr = rar2_removexattr,
#endif
};

struct work_task_data {
        struct fuse *fuse;
        int mt;
        volatile int work_task_exited;
        int status;
};

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *work_task(void *data)
{
        struct work_task_data *wdt = (struct work_task_data *)data;
        wdt->status = wdt->mt ? fuse_loop_mt(wdt->fuse) : fuse_loop(wdt->fuse);
        wdt->work_task_exited = 1;
        pthread_exit(NULL);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int work(struct fuse_args *args)
{
        struct work_task_data wdt;

        /* For portability, explicitly create threads in a joinable state */
        pthread_attr_init(&thread_attr);
        pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);

#if defined ( HAVE_SCHED_SETAFFINITY ) && defined ( HAVE_CPU_SET_T )
        cpu_set_t cpu_mask_saved;
        if (OPT_SET(OPT_KEY_NO_SMP)) {
                cpu_set_t cpu_mask;
                CPU_ZERO(&cpu_mask);
                CPU_SET(0, &cpu_mask);
                if (sched_getaffinity(0, sizeof(cpu_set_t), &cpu_mask_saved))
                        perror("sched_getaffinity");
                else if (sched_setaffinity(0, sizeof(cpu_set_t), &cpu_mask))
                        perror("sched_setaffinity");
        }
#endif

        /* The below callbacks depend on mount type */
        if (mount_type == MOUNT_FOLDER) {
                rar2_operations.getattr         = rar2_getattr;
                rar2_operations.readdir         = rar2_readdir;
                rar2_operations.create          = rar2_create;
                rar2_operations.rename          = rar2_rename;
                rar2_operations.mknod           = rar2_mknod;
                rar2_operations.unlink          = rar2_unlink;
                rar2_operations.mkdir           = rar2_mkdir;
                rar2_operations.rmdir           = rar2_rmdir;
                rar2_operations.write           = rar2_write;
                rar2_operations.truncate        = rar2_truncate;
                rar2_operations.chmod           = rar2_chmod;
                rar2_operations.chown           = rar2_chown;
        } else {
                rar2_operations.getattr         = rar2_getattr2;
                rar2_operations.readdir         = rar2_readdir2;
                rar2_operations.create          = (void *)rar2_eperm_stub;
                rar2_operations.rename          = (void *)rar2_eperm_stub;
                rar2_operations.mknod           = (void *)rar2_eperm_stub;
                rar2_operations.unlink          = (void *)rar2_eperm_stub;
                rar2_operations.mkdir           = (void *)rar2_eperm_stub;
                rar2_operations.rmdir           = (void *)rar2_eperm_stub;
                rar2_operations.write           = (void *)rar2_eperm_stub;
                rar2_operations.truncate        = (void *)rar2_eperm_stub;
                rar2_operations.chmod           = (void *)rar2_eperm_stub;
                rar2_operations.chown           = (void *)rar2_eperm_stub;
        }

        struct fuse *f;
        pthread_t t;
        char *mp;
        int mt;

        f = fuse_setup(args->argc, args->argv, &rar2_operations,
                                sizeof(rar2_operations), &mp, &mt, NULL);
        if (!f)
            return -1;
        wdt.fuse = f;
        wdt.mt = mt;
        wdt.work_task_exited = 0;
        wdt.status = 0;
        pthread_create(&t, &thread_attr, work_task, (void *)&wdt);

        /*
         * This is a workaround for an issue with fuse_loop() that does
         * not always release properly at reception of SIGINT.
         * But this is really what we want since this thread should not
         * block blindly without some user control.
         */
        while (!fuse_exited(f) && !wdt.work_task_exited) {
                sleep(1);
                ++rar2_ticks;
        }
        if (!wdt.work_task_exited)
                pthread_kill(t, SIGINT);        /* terminate nicely */

        fs_terminated = 1;
        pthread_join(t, NULL);
        fuse_teardown(f, mp);

#if defined ( HAVE_SCHED_SETAFFINITY ) && defined ( HAVE_CPU_SET_T )
        if (OPT_SET(OPT_KEY_NO_SMP)) {
                if (sched_setaffinity(0, sizeof(cpu_set_t), &cpu_mask_saved))
                        perror("sched_setaffinity");
        }
#endif

        pthread_attr_destroy(&thread_attr);

        return wdt.status;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void print_version()
{
#ifdef SVNREV
        printf("rar2fs v%u.%u.%u-svnr%d (DLL version %d)    Copyright (C) 2009-2013 Hans Beckerus\n",
#else
        printf("rar2fs v%u.%u.%u (DLL version %d)    Copyright (C) 2009-2013 Hans Beckerus\n",
#endif
               RAR2FS_MAJOR_VER,
               RAR2FS_MINOR_VER, RAR2FS_PATCH_LVL,
#ifdef SVNREV
               SVNREV,
#endif
               RARGetDllVersion());
        printf("This program comes with ABSOLUTELY NO WARRANTY.\n"
               "This is free software, and you are welcome to redistribute it under\n"
               "certain conditions; see <http://www.gnu.org/licenses/> for details.\n");
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void print_help()
{
        printf("\nrar2fs options:\n");
        printf("    --img-type=E1[;E2...]   additional image file type extensions beyond the default [.iso;.img;.nrg]\n");
        printf("    --show-comp-img\t    show image file types also for compressed archives\n");
        printf("    --preopen-img\t    prefetch volume file descriptors for image file types\n");
        printf("    --fake-iso[=E1[;E2...]] fake .iso extension for specified image file types\n");
        printf("    --exclude=F1[;F2...]    exclude file filter\n");
        printf("    --seek-length=n\t    set number of volume files that are traversed in search for headers [0=All]\n");
        printf("    --flat-only\t\t    only expand first level of nested RAR archives\n");
        printf("    --iob-size=n\t    I/O buffer size in 'power of 2' MiB (1,2,4,8, etc.) [4]\n");
        printf("    --hist-size=n\t    I/O buffer history size as a percentage (0-75) of total buffer size [50]\n");
        printf("    --save-eof\t\t    force creation of .r2i files (end-of-file chunk)\n");
#ifdef ENABLE_OBSOLETE_ARGS
        printf("    --unrar-path=PATH\t    path to external unrar binary (overide libunrar)\n");
        printf("    --no-password\t    disable password file support\n");
#endif
        printf("    --no-lib-check\t    disable validation of library version(s)\n");
#ifndef USE_STATIC_IOB_
        printf("    --no-expand-cbr\t    do not expand comic book RAR archives\n");
#endif
#if defined ( HAVE_SCHED_SETAFFINITY ) && defined ( HAVE_CPU_SET_T )
        printf("    --no-smp\t\t    disable SMP support (bind to CPU #0)\n");
#endif
}

/* FUSE API specific keys continue where 'optdb' left off */
enum {
        OPT_KEY_HELP = OPT_KEY_END,
        OPT_KEY_VERSION,
};

static struct fuse_opt rar2fs_opts[] = {
        FUSE_OPT_KEY("-V",              OPT_KEY_VERSION),
        FUSE_OPT_KEY("--version",       OPT_KEY_VERSION),
        FUSE_OPT_KEY("-h",              OPT_KEY_HELP),
        FUSE_OPT_KEY("--help",          OPT_KEY_HELP),
        FUSE_OPT_END
};

static struct option longopts[] = {
        {"show-comp-img",     no_argument, NULL, OPT_ADDR(OPT_KEY_SHOW_COMP_IMG)},
        {"preopen-img",       no_argument, NULL, OPT_ADDR(OPT_KEY_PREOPEN_IMG)},
        {"fake-iso",    optional_argument, NULL, OPT_ADDR(OPT_KEY_FAKE_ISO)},
        {"exclude",     required_argument, NULL, OPT_ADDR(OPT_KEY_EXCLUDE)},
        {"seek-length", required_argument, NULL, OPT_ADDR(OPT_KEY_SEEK_LENGTH)},
#ifdef ENABLE_OBSOLETE_ARGS
        {"unrar-path",  required_argument, NULL, OPT_ADDR(OPT_KEY_UNRAR_PATH)},
        {"no-password",       no_argument, NULL, OPT_ADDR(OPT_KEY_NO_PASSWD)},
#endif
        /* 
         * --seek-depth=n is obsolete and replaced by --flat-only
         * Provided here only for backwards compatibility. 
         */
        {"seek-depth",  required_argument, NULL, OPT_ADDR(OPT_KEY_SEEK_DEPTH)},
#if defined ( HAVE_SCHED_SETAFFINITY ) && defined ( HAVE_CPU_SET_T )
        {"no-smp",            no_argument, NULL, OPT_ADDR(OPT_KEY_NO_SMP)},
#endif
        {"img-type",    required_argument, NULL, OPT_ADDR(OPT_KEY_IMG_TYPE)},
        {"no-lib-check",      no_argument, NULL, OPT_ADDR(OPT_KEY_NO_LIB_CHECK)},
#ifndef USE_STATIC_IOB_
        {"hist-size",   required_argument, NULL, OPT_ADDR(OPT_KEY_HIST_SIZE)},
        {"iob-size",    required_argument, NULL, OPT_ADDR(OPT_KEY_BUF_SIZE)},
#endif
        {"save-eof",          no_argument, NULL, OPT_ADDR(OPT_KEY_SAVE_EOF)},
        {"no-expand-cbr",     no_argument, NULL, OPT_ADDR(OPT_KEY_NO_EXPAND_CBR)},
        {"flat-only",         no_argument, NULL, OPT_ADDR(OPT_KEY_FLAT_ONLY)},
        {NULL,                          0, NULL, 0}
};

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2fs_opt_proc(void *data, const char *arg, int key,
                struct fuse_args *outargs)
{
        const char *const argv[2] = {outargs->argv[0], arg};

        (void)data;             /* touch */

        switch (key) {
        case FUSE_OPT_KEY_NONOPT:
                if (!OPT_SET(OPT_KEY_SRC)) {
                        optdb_save(OPT_KEY_SRC, arg);
                        return 0;
                }
                if (!OPT_SET(OPT_KEY_DST)) {
                        optdb_save(OPT_KEY_DST, arg);
                        return 0;
                }
                usage(outargs->argv[0]);
                return -1;

        case FUSE_OPT_KEY_OPT:
                optind=0;
                opterr=1;
                int opt = getopt_long(2, (char *const*)argv, "dfs", longopts, NULL);
                if (opt == '?')
                        return -1;
                if (opt >= OPT_ADDR(0)) {
                        if (!optdb_save(OPT_ID(opt), optarg))
                                return 0;
                        usage(outargs->argv[0]);
                        return -1;
                }
                return 1;

        case OPT_KEY_HELP:
                fprintf(stderr,
                        "usage: %s source mountpoint [options]\n"
                        "\n"
                        "general options:\n"
                        "    -o opt,[opt...]        mount options\n"
                        "    -h   --help            print help\n"
                        "    -V   --version         print version\n"
                        "\n", outargs->argv[0]);
                fuse_opt_add_arg(outargs, "-ho");
                fuse_main(outargs->argc, outargs->argv,
                                (struct fuse_operations*)NULL, NULL);
                print_help();
                exit(0);

        case OPT_KEY_VERSION:
                print_version();
                fuse_opt_add_arg(outargs, "--version");
                fuse_main(outargs->argc, outargs->argv,
                                (struct fuse_operations*)NULL, NULL);
                exit(0);

        default:
                break;
        }

        return 1;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
int main(int argc, char *argv[])
{
        int res = 0;

#ifdef HAVE_SETLOCALE
        setlocale(LC_CTYPE, "");
#endif

#ifdef HAVE_UMASK
        umask_ = umask(0);
        umask(umask_);
#endif

        /*openlog("rarfs2", LOG_NOWAIT|LOG_PID, 0);*/
        optdb_init();

        long ps = -1;
#if defined ( _SC_PAGE_SIZE )
        ps = sysconf(_SC_PAGE_SIZE);
#else
#if defined ( _SC_PAGESIZE )
        ps = sysconf(_SC_PAGESIZE);
#endif
#endif
        if (ps != -1)
                page_size = ps;
        else
                page_size = 4096;

        struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
        if (fuse_opt_parse(&args, NULL, rar2fs_opts, rar2fs_opt_proc))
                return -1;

        /* Check src/dst path */
        if (OPT_SET(OPT_KEY_SRC) && OPT_SET(OPT_KEY_DST)) {
                char *dst_path = NULL;
                char *src_path = NULL;
                if (check_paths(argv[0],
                                OPT_STR(OPT_KEY_SRC, 0),
                                OPT_STR(OPT_KEY_DST, 0),
                                &src_path, &dst_path, 1))
                        return -1;

                optdb_save(OPT_KEY_SRC, src_path);
                optdb_save(OPT_KEY_DST, dst_path);
                free(src_path);
                free(dst_path);
        } else {
                usage(argv[0]);
                return 0;
        }

        /* Check I/O buffer and history size */
        if (check_iob(argv[0], 1))
                return -1;

        /* Check library versions */
        if (!OPT_SET(OPT_KEY_NO_LIB_CHECK)) {
                if (check_libunrar(1) || check_libfuse(1))
                        return -1;
        }

        fuse_opt_add_arg(&args, "-s");
        fuse_opt_add_arg(&args, "-osync_read,fsname=rar2fs,subtype=rar2fs,default_permissions");
        if (OPT_SET(OPT_KEY_DST))
                fuse_opt_add_arg(&args, OPT_STR(OPT_KEY_DST, 0));

        /*
         * All static setup is ready, the rest is taken from the configuration.
         * Continue in work() function which will not return until the process
         * is about to terminate.
         */
        res = work(&args);

        /* Clean up what has not already been taken care of */
        fuse_opt_free_args(&args);
        optdb_destroy();
        if (mount_type == MOUNT_ARCHIVE)
                dir_list_free(arch_list);

        return res;
}