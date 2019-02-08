
#include <sys/mman.h>
#include <errno.h>

#define DBG_SUBSYS S_YFSCDS

#include "sdfs_lib.h"
#include "ylib.h"
#include "replica.h"
#include "net_global.h"
#include "schedule.h"
#include "nodeid.h"
#include "md_lib.h"
#include "diskio.h"
#include "io_analysis.h"
#include "sdfs_aio.h"
#include "core.h"
#include "dbg.h"

static int __seq__ = 0;

static inline void __disk_build_chkpath(char *path, const chkid_t *chkid, int level)
{
        char cpath[MAX_PATH_LEN];

        (void) cascade_id2path(cpath, MAX_PATH_LEN, chkid->id);

        (void) snprintf(path, MAX_PATH_LEN, "%s/disk/%u/%s_v%llu/%u",
                        ng.home, level, cpath, (LLU)chkid->volid, chkid->idx);

}

static int __replica_getfd__(va_list ap)
{
        int ret, fd;
        char path[MAX_PATH_LEN];
        const chkid_t *chkid = va_arg(ap, const chkid_t *);
        int *_fd = va_arg(ap, int *);
        int flag = va_arg(ap, int);

        va_end(ap);
        
        ANALYSIS_BEGIN(1);
        
        chkid2path(chkid, path);

        ret = path_validate(path, YLIB_NOTDIR, YLIB_DIRCREATE);
        if (ret)
                GOTO(err_ret, ret);

        DBUG("path %s\n", path);
        
        fd = open(path, flag);
        if (fd < 0) {
                ret = errno;
                DWARN("open %s fail\n", path);
                GOTO(err_ret, ret);
        }

        ANALYSIS_QUEUE(1, IO_WARN, NULL);
        
        *_fd = fd;
        
        return 0;
err_ret:
        return ret;
}


static int __replica_getfd(const chkid_t *chkid, int *_fd, int flag)
{
        return schedule_newthread(SCHE_THREAD_REPLICA, ++__seq__, FALSE,
                                  "getfd", -1, __replica_getfd__,
                                  chkid, _fd, flag);
}


static void __replica_release(int fd)
{
        close(fd);
}

static void __callback(void *_iocb, void *_retval)
{
        struct iocb *iocb = _iocb;      
        task_t *task = iocb->data;
        int *retval = _retval;
  
        schedule_resume(task, *retval, NULL);
}

#define SECTOR_SIZE 512

static int IO_FUNC __replica_write_sync(const io_t *io, const buffer_t *buf)
{
        int ret, fd, iov_count;
        task_t task;
        struct iocb iocb;
        struct iovec iov[Y_MSG_MAX / BUFFER_SEG_SIZE + 1];
        
        ret = __replica_getfd(&io->id, &fd, O_CREAT | O_SYNC | O_RDWR);
        if (ret)
                GOTO(err_ret, ret);

        iov_count = Y_MSG_MAX / BUFFER_SEG_SIZE + 1;
        ret = mbuffer_trans(iov, &iov_count, buf);
        //DBUG("ret %u %u\n", ret, buf->len);
        YASSERT(ret == (int)buf->len);

        io_prep_pwritev(&iocb, fd, iov, iov_count, io->offset);

        iocb.aio_reqprio = 0;
        task = schedule_task_get();
        iocb.data = &task;

        ret = diskio_submit(&iocb, __callback);
        if (ret)
                GOTO(err_fd, ret);

        ret = schedule_yield("aio_commit", NULL, NULL);
        if (ret < 0) {
                ret = -ret;
                GOTO(err_fd, ret);
        }

        __replica_release(fd);
        
        if (ret != (int)buf->len) {
                ret = EIO;
                GOTO(err_ret, ret);
        }
        
        return 0;
err_fd:
        __replica_release(fd);
err_ret:
        return ret;
}

static int IO_FUNC __replica_write_direct(const io_t *io, const buffer_t *buf)
{
        int ret, fd, iov_count;
        task_t task;
        struct iocb iocb;
        struct iovec iov[Y_MSG_MAX / BUFFER_SEG_SIZE + 1];
        buffer_t tmp;

        mbuffer_init(&tmp, 0);
        mbuffer_clone1(&tmp, buf);
        
        ret = __replica_getfd(&io->id, &fd, O_CREAT | O_DIRECT | O_RDWR);
        if (ret)
                GOTO(err_ret, ret);

        iov_count = Y_MSG_MAX / BUFFER_SEG_SIZE + 1;
        ret = mbuffer_trans(iov, &iov_count, &tmp);
        //DBUG("ret %u %u\n", ret, buf->len);
        YASSERT(ret == (int)buf->len);

        for (int i=0; i < iov_count; i++) {
                YASSERT(iov[i].iov_len % PAGE_SIZE == 0);
                YASSERT((uint64_t)iov[i].iov_base % PAGE_SIZE == 0);
        }
        
        io_prep_pwritev(&iocb, fd, iov, iov_count, io->offset);

        iocb.aio_reqprio = 0;
        task = schedule_task_get();
        iocb.data = &task;

#if 1
        ret = aio_commit(&iocb, buf->len, 0, AIO_MODE_DIRECT);
        if (ret)
                GOTO(err_fd, ret);
#else
        ret = diskio_submit(&iocb, __callback);
        if (ret)
                GOTO(err_fd, ret);
#endif

        ret = schedule_yield("aio_commit", NULL, NULL);
        if (ret < 0) {
                ret = -ret;
                GOTO(err_fd, ret);
        }
        
        mbuffer_free(&tmp);
        __replica_release(fd);

        if (ret != (int)buf->len) {
                ret = EIO;
                GOTO(err_ret, ret);
        }
        
        return 0;
err_fd:
        __replica_release(fd);
err_ret:
        mbuffer_free(&tmp);
        return ret;
}

int IO_FUNC __replica_write__(const io_t *io, const buffer_t *buf)
{
        int ret;

        ret = io_analysis(ANALYSIS_IO_WRITE, io->size);
        if (ret)
                GOTO(err_ret, ret);
        
        ANALYSIS_BEGIN(0);
        CORE_ANALYSIS_BEGIN(1);
        
        YASSERT(schedule_running());

        DBUG("write "CHKID_FORMAT"\n", CHKID_ARG(&io->id));

        if (io->offset % SECTOR_SIZE == 0 &&
            io->size % SECTOR_SIZE == 0 && 0) {
                ret = __replica_write_direct(io, buf);
                if (ret)
                        GOTO(err_ret, ret);
        } else {
                ret = __replica_write_sync(io, buf);
                if (ret)
                        GOTO(err_ret, ret);
        }

        DBUG("write "CHKID_FORMAT" finish\n", CHKID_ARG(&io->id));
        
        ANALYSIS_QUEUE(0, IO_WARN, NULL);
        CORE_ANALYSIS_UPDATE(1, IO_WARN, "write");       
        
        return 0;
err_ret:
        return ret;
}

static int IO_FUNC __replica_read_direct(const io_t *io, buffer_t *buf)
{
        int ret, iov_count;
        int fd;
        task_t task;
        struct iocb iocb;
        struct iovec iov[Y_MSG_MAX / BUFFER_SEG_SIZE + 1];

        DBUG("read "CHKID_FORMAT"\n", CHKID_ARG(&io->id));
        
        ret = __replica_getfd(&io->id, &fd, O_RDONLY | O_DIRECT);
        if (ret)
                GOTO(err_ret, ret);

        YASSERT(buf->len == 0);
        mbuffer_init(buf, io->size);
        iov_count = Y_MSG_MAX / BUFFER_SEG_SIZE + 1;
        ret = mbuffer_trans(iov, &iov_count, buf);
        DBUG("ret %u %u\n", ret, buf->len);
        YASSERT(ret == (int)buf->len);

        io_prep_preadv(&iocb, fd, iov, iov_count, io->offset);

        iocb.aio_reqprio = 0;
        task = schedule_task_get();
        iocb.data = &task;

#if 1
        ret = aio_commit(&iocb, buf->len, 0, AIO_MODE_DIRECT);
        if (ret)
                GOTO(err_fd, ret);
#else 
        ret = diskio_submit(&iocb, __callback);
        if (ret)
                GOTO(err_fd, ret);
#endif

        ret = schedule_yield("aio_commit", NULL, NULL);
        if (ret < 0) {
                ret = -ret;
                GOTO(err_fd, ret);
        }

        __replica_release(fd);

        return 0;
err_fd:
        __replica_release(fd);
err_ret:
        return ret;
}

static int IO_FUNC __replica_read_sync(const io_t *io, buffer_t *buf)
{
        int ret, iov_count;
        int fd;
        task_t task;
        struct iocb iocb;
        struct iovec iov[Y_MSG_MAX / BUFFER_SEG_SIZE + 1];

        DBUG("read "CHKID_FORMAT"\n", CHKID_ARG(&io->id));
        
        ret = __replica_getfd(&io->id, &fd, O_RDONLY);
        if (ret)
                GOTO(err_ret, ret);

        YASSERT(buf->len == 0);
        mbuffer_init(buf, io->size);
        iov_count = Y_MSG_MAX / BUFFER_SEG_SIZE + 1;
        ret = mbuffer_trans(iov, &iov_count, buf);
        DBUG("ret %u %u\n", ret, buf->len);
        YASSERT(ret == (int)buf->len);

        io_prep_preadv(&iocb, fd, iov, iov_count, io->offset);

        iocb.aio_reqprio = 0;
        task = schedule_task_get();
        iocb.data = &task;

        ret = diskio_submit(&iocb, __callback);
        if (ret)
                GOTO(err_fd, ret);

        ret = schedule_yield("aio_commit", NULL, NULL);
        if (ret < 0) {
                ret = -ret;
                GOTO(err_fd, ret);
        }
        
        __replica_release(fd);
        
        return 0;
err_fd:
        __replica_release(fd);
err_ret:
        return ret;
}

int IO_FUNC __replica_read__(const io_t *io, buffer_t *buf)
{
        int ret;

        ret = io_analysis(ANALYSIS_IO_READ, io->size);
        if (ret)
                GOTO(err_ret, ret);
        
        ANALYSIS_BEGIN(0);
        CORE_ANALYSIS_BEGIN(1);
        
        YASSERT(schedule_running());

        DBUG("read "CHKID_FORMAT"\n", CHKID_ARG(&io->id));

        if (io->offset % SECTOR_SIZE == 0 &&
            io->size % SECTOR_SIZE == 0 && 0) {
                ret = __replica_read_direct(io, buf);
                if (ret)
                        GOTO(err_ret, ret);
        } else {
                ret = __replica_read_sync(io, buf);
                if (ret)
                        GOTO(err_ret, ret);
        }

        if (ret < (int)buf->len) {
                mbuffer_droptail(buf, buf->len - ret);
        }

        ANALYSIS_QUEUE(0, IO_WARN, NULL); 
        CORE_ANALYSIS_UPDATE(1, IO_WARN, "read");       
        
        return 0;
err_ret:
        return ret;
}

int IO_FUNC __replica_read(va_list ap)
{
        const io_t *io = va_arg(ap, const io_t *);
        buffer_t*buf = va_arg(ap, buffer_t *);

        return __replica_read__(io, buf);
        
}

int IO_FUNC replica_read(const io_t *io, buffer_t *buf)
{
        int ret;

        ret = core_request(io->id.id, -1, "replica_read", __replica_read, io, buf);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int IO_FUNC __replica_write(va_list ap)
{
        const io_t *io = va_arg(ap, const io_t *);
        const buffer_t*buf = va_arg(ap, buffer_t *);

        return __replica_write__(io, buf);
}

int IO_FUNC replica_write(const io_t *io, const buffer_t *buf)
{
        int ret;

        ret = core_request(io->id.id, -1, "replica_write", __replica_write, io, buf);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

struct sche_thread_ops replica_ops = {
        .type           = SCHE_THREAD_REPLICA,
        .begin_trans    = NULL,
        .commit_trans   = NULL,
};

int replica_init()
{
        return sche_thread_ops_register(&replica_ops, replica_ops.type, 8);
}
