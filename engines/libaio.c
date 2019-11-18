/*
 * libaio engine
 *
 * IO engine using the Linux native aio interface.
 *
 */
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <libaio.h>

#include "../fio.h"
#include "../lib/pow2.h"
#include "../optgroup.h"
#include "../lib/memalign.h"

#ifndef IOCB_FLAG_HIPRI
#define IOCB_FLAG_HIPRI	(1 << 2)
#endif

#ifndef IOCTX_FLAG_USERIOCB
#define IOCTX_FLAG_USERIOCB	(1 << 0)
#endif
#ifndef IOCTX_FLAG_IOPOLL
#define IOCTX_FLAG_IOPOLL	(1 << 1)
#endif
#ifndef IOCTX_FLAG_FIXEDBUFS
#define IOCTX_FLAG_FIXEDBUFS	(1 << 2)
#endif

// zhou: used by aws/ebs/ld

static int fio_libaio_commit(struct thread_data *td);

struct libaio_data {
	io_context_t aio_ctx;
	struct io_event *aio_events;
	struct iocb **iocbs;
	struct io_u **io_us;

	struct iocb *user_iocbs;
	struct io_u **io_u_index;

	/*
	 * Basic ring buffer. 'head' is incremented in _queue(), and
	 * 'tail' is incremented in _commit(). We keep 'queued' so
	 * that we know if the ring is full or empty, when
	 * 'head' == 'tail'. 'entries' is the ring size, and
	 * 'is_pow2' is just an optimization to use AND instead of
	 * modulus to get the remainder on ring increment.
	 */
	int is_pow2;
	unsigned int entries;
	unsigned int queued;
	unsigned int head;
	unsigned int tail;
};

struct libaio_options {
	void *pad;
	unsigned int userspace_reap;
	unsigned int hipri;
	unsigned int useriocb;
	unsigned int fixedbufs;
};

static struct fio_option options[] = {
	{
		.name	= "userspace_reap",
		.lname	= "Libaio userspace reaping",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct libaio_options, userspace_reap),
		.help	= "Use alternative user-space reap implementation",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBAIO,
	},
	{
		.name	= "hipri",
		.lname	= "High Priority",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct libaio_options, hipri),
		.help	= "Use polled IO completions",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBAIO,
	},
	{
		.name	= "useriocb",
		.lname	= "User IOCBs",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct libaio_options, useriocb),
		.help	= "Use user mapped IOCBs",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBAIO,
	},
	{
		.name	= "fixedbufs",
		.lname	= "Fixed (pre-mapped) IO buffers",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct libaio_options, fixedbufs),
		.help	= "Pre map IO buffers",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBAIO,
	},
	{
		.name	= NULL,
	},
};

static inline void ring_inc(struct libaio_data *ld, unsigned int *val,
			    unsigned int add)
{
	if (ld->is_pow2)
		*val = (*val + add) & (ld->entries - 1);
	else
		*val = (*val + add) % ld->entries;
}

static int fio_libaio_prep(struct thread_data fio_unused *td, struct io_u *io_u)
{
	struct libaio_data *ld = td->io_ops_data;
	struct fio_file *f = io_u->file;
	struct libaio_options *o = td->eo;
	struct iocb *iocb;

	if (o->useriocb)
		iocb = &ld->user_iocbs[io_u->index];
	else
		iocb = &io_u->iocb;

	iocb->u.c.flags = 0;

	if (io_u->ddir == DDIR_READ) {
		io_prep_pread(iocb, f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);
		if (o->hipri)
			iocb->u.c.flags |= IOCB_FLAG_HIPRI;
	} else if (io_u->ddir == DDIR_WRITE) {
		io_prep_pwrite(iocb, f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);
		if (o->hipri)
			iocb->u.c.flags |= IOCB_FLAG_HIPRI;
	} else if (ddir_sync(io_u->ddir))
		io_prep_fsync(iocb, f->fd);

	return 0;
}

static struct io_u *fio_libaio_event(struct thread_data *td, int event)
{
	struct libaio_data *ld = td->io_ops_data;
	struct libaio_options *o = td->eo;
	struct io_event *ev;
	struct io_u *io_u;

	ev = ld->aio_events + event;
	if (o->useriocb) {
		int index = (int) (uintptr_t) ev->obj;
		io_u = ld->io_u_index[index];
	} else
		io_u = container_of(ev->obj, struct io_u, iocb);

	if (ev->res != io_u->xfer_buflen) {
		if (ev->res > io_u->xfer_buflen)
			io_u->error = -ev->res;
		else
			io_u->resid = io_u->xfer_buflen - ev->res;
	} else
		io_u->error = 0;

	return io_u;
}

struct aio_ring {
	unsigned id;		 /** kernel internal index number */
	unsigned nr;		 /** number of io_events */
	unsigned head;
	unsigned tail;

	unsigned magic;
	unsigned compat_features;
	unsigned incompat_features;
	unsigned header_length;	/** size of aio_ring */

	struct io_event events[0];
};

#define AIO_RING_MAGIC	0xa10a10a1

static int user_io_getevents(io_context_t aio_ctx, unsigned int max,
			     struct io_event *events)
{
	long i = 0;
	unsigned head;
	struct aio_ring *ring = (struct aio_ring*) aio_ctx;

	while (i < max) {
		head = ring->head;

		if (head == ring->tail) {
			/* There are no more completions */
			break;
		} else {
			/* There is another completion to reap */
			events[i] = ring->events[head];
			read_barrier();
			ring->head = (head + 1) % ring->nr;
			i++;
		}
	}

	return i;
}

static int fio_libaio_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	struct libaio_data *ld = td->io_ops_data;
	struct libaio_options *o = td->eo;
	unsigned actual_min = td->o.iodepth_batch_complete_min == 0 ? 0 : min;
	struct timespec __lt, *lt = NULL;
	int r, events = 0;

	if (t) {
		__lt = *t;
		lt = &__lt;
	}

	do {
		if (o->userspace_reap == 1
		    && actual_min == 0
		    && ((struct aio_ring *)(ld->aio_ctx))->magic
				== AIO_RING_MAGIC) {
			r = user_io_getevents(ld->aio_ctx, max,
				ld->aio_events + events);
		} else {
			r = io_getevents(ld->aio_ctx, actual_min,
				max, ld->aio_events + events, lt);
		}
		if (r > 0)
			events += r;
		else if ((min && r == 0) || r == -EAGAIN) {
			fio_libaio_commit(td);
			if (actual_min)
				usleep(10);
		} else if (r != -EINTR)
			break;
	} while (events < min);

	return r < 0 ? r : events;
}

static enum fio_q_status fio_libaio_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct libaio_data *ld = td->io_ops_data;
	struct libaio_options *o = td->eo;

	fio_ro_check(td, io_u);

	if (ld->queued == td->o.iodepth)
		return FIO_Q_BUSY;

	/*
	 * fsync is tricky, since it can fail and we need to do it
	 * serialized with other io. the reason is that linux doesn't
	 * support aio fsync yet. So return busy for the case where we
	 * have pending io, to let fio complete those first.
	 */
	if (ddir_sync(io_u->ddir)) {
		if (ld->queued)
			return FIO_Q_BUSY;

		do_io_u_sync(td, io_u);
		return FIO_Q_COMPLETED;
	}

	if (io_u->ddir == DDIR_TRIM) {
		if (ld->queued)
			return FIO_Q_BUSY;

		do_io_u_trim(td, io_u);
		io_u_mark_submit(td, 1);
		io_u_mark_complete(td, 1);
		return FIO_Q_COMPLETED;
	}

	if (o->useriocb)
		ld->iocbs[ld->head] = (struct iocb *) (uintptr_t) io_u->index;
	else
		ld->iocbs[ld->head] = &io_u->iocb;

	ld->io_us[ld->head] = io_u;
	ring_inc(ld, &ld->head, 1);
	ld->queued++;
	return FIO_Q_QUEUED;
}

static void fio_libaio_queued(struct thread_data *td, struct io_u **io_us,
			      unsigned int nr)
{
	struct timespec now;
	unsigned int i;

	if (!fio_fill_issue_time(td))
		return;

	fio_gettime(&now, NULL);

	for (i = 0; i < nr; i++) {
		struct io_u *io_u = io_us[i];

		memcpy(&io_u->issue_time, &now, sizeof(now));
		io_u_queued(td, io_u);
	}
}

static int fio_libaio_commit(struct thread_data *td)
{
	struct libaio_data *ld = td->io_ops_data;
	struct iocb **iocbs;
	struct io_u **io_us;
	struct timespec ts;
	int ret, wait_start = 0;

	if (!ld->queued)
		return 0;

	do {
		long nr = ld->queued;

		nr = min((unsigned int) nr, ld->entries - ld->tail);
		io_us = ld->io_us + ld->tail;
		iocbs = ld->iocbs + ld->tail;

		ret = io_submit(ld->aio_ctx, nr, iocbs);
		if (ret > 0) {
			fio_libaio_queued(td, io_us, ret);
			io_u_mark_submit(td, ret);

			ld->queued -= ret;
			ring_inc(ld, &ld->tail, ret);
			ret = 0;
			wait_start = 0;
		} else if (ret == -EINTR || !ret) {
			if (!ret)
				io_u_mark_submit(td, ret);
			wait_start = 0;
			continue;
		} else if (ret == -EAGAIN) {
			/*
			 * If we get EAGAIN, we should break out without
			 * error and let the upper layer reap some
			 * events for us. If we have no queued IO, we
			 * must loop here. If we loop for more than 30s,
			 * just error out, something must be buggy in the
			 * IO path.
			 */
			if (ld->queued) {
				ret = 0;
				break;
			}
			if (!wait_start) {
				fio_gettime(&ts, NULL);
				wait_start = 1;
			} else if (mtime_since_now(&ts) > 30000) {
				log_err("fio: aio appears to be stalled, giving up\n");
				break;
			}
			usleep(1);
			continue;
		} else if (ret == -ENOMEM) {
			/*
			 * If we get -ENOMEM, reap events if we can. If
			 * we cannot, treat it as a fatal event since there's
			 * nothing we can do about it.
			 */
			if (ld->queued)
				ret = 0;
			break;
		} else
			break;
	} while (ld->queued);

	return ret;
}

static int fio_libaio_cancel(struct thread_data *td, struct io_u *io_u)
{
	struct libaio_data *ld = td->io_ops_data;

	return io_cancel(ld->aio_ctx, &io_u->iocb, ld->aio_events);
}

static void fio_libaio_cleanup(struct thread_data *td)
{
	struct libaio_data *ld = td->io_ops_data;

	if (ld) {
		/*
		 * Work-around to avoid huge RCU stalls at exit time. If we
		 * don't do this here, then it'll be torn down by exit_aio().
		 * But for that case we can parallellize the freeing, thus
		 * speeding it up a lot.
		 */
		if (!(td->flags & TD_F_CHILD))
			io_destroy(ld->aio_ctx);
		free(ld->aio_events);
		free(ld->iocbs);
		free(ld->io_us);
		if (ld->user_iocbs) {
			size_t size = td->o.iodepth * sizeof(struct iocb);
			fio_memfree(ld->user_iocbs, size, false);
		}
		free(ld);
	}
}

static int fio_libaio_old_queue_init(struct libaio_data *ld, unsigned int depth,
				     bool hipri, bool useriocb, bool fixedbufs)
{
	if (hipri) {
		log_err("fio: polled aio not available on your platform\n");
		return 1;
	}
	if (useriocb) {
		log_err("fio: user mapped iocbs not available on your platform\n");
		return 1;
	}
	if (fixedbufs) {
		log_err("fio: fixed buffers not available on your platform\n");
		return 1;
	}

	return io_queue_init(depth, &ld->aio_ctx);
}

static int fio_libaio_queue_init(struct libaio_data *ld, unsigned int depth,
				 bool hipri, bool useriocb, bool fixedbufs)
{
#ifdef __NR_sys_io_setup2
	int ret, flags = 0;

	if (hipri)
		flags |= IOCTX_FLAG_IOPOLL;
	if (useriocb)
		flags |= IOCTX_FLAG_USERIOCB;
	if (fixedbufs)
		flags |= IOCTX_FLAG_FIXEDBUFS;

	ret = syscall(__NR_sys_io_setup2, depth, flags, ld->user_iocbs,
			&ld->aio_ctx);
	if (!ret)
		return 0;
	/* fall through to old syscall */
#endif
	return fio_libaio_old_queue_init(ld, depth, hipri, useriocb, fixedbufs);
}

static int fio_libaio_post_init(struct thread_data *td)
{
	struct libaio_data *ld = td->io_ops_data;
	struct libaio_options *o = td->eo;
	struct io_u *io_u;
	struct iocb *iocb;
	int err = 0;

	if (o->fixedbufs) {
		int i;

		for (i = 0; i < td->o.iodepth; i++) {
			io_u = ld->io_u_index[i];
			iocb = &ld->user_iocbs[i];
			iocb->u.c.buf = io_u->buf;
			iocb->u.c.nbytes = td_max_bs(td);
		}
	}

	err = fio_libaio_queue_init(ld, td->o.iodepth, o->hipri, o->useriocb,
					o->fixedbufs);
	if (err) {
		td_verror(td, -err, "io_queue_init");
		return 1;
	}

	return 0;
}

static int fio_libaio_init(struct thread_data *td)
{
	struct libaio_options *o = td->eo;
	struct libaio_data *ld;

	ld = calloc(1, sizeof(*ld));

	if (o->useriocb) {
		size_t size;

		ld->io_u_index = calloc(td->o.iodepth, sizeof(struct io_u *));
		size = td->o.iodepth * sizeof(struct iocb);
		ld->user_iocbs = fio_memalign(page_size, size, false);
		memset(ld->user_iocbs, 0, size);
	}

	ld->entries = td->o.iodepth;
	ld->is_pow2 = is_power_of_2(ld->entries);
	ld->aio_events = calloc(ld->entries, sizeof(struct io_event));
	ld->iocbs = calloc(ld->entries, sizeof(struct iocb *));
	ld->io_us = calloc(ld->entries, sizeof(struct io_u *));

	td->io_ops_data = ld;
	return 0;
}

static int fio_libaio_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct libaio_options *o = td->eo;

	if (o->useriocb) {
		struct libaio_data *ld = td->io_ops_data;

		ld->io_u_index[io_u->index] = io_u;
	}

	return 0;
}

// zhou: linux kernel aio, NOT glibc aio.
static struct ioengine_ops ioengine = {
	.name			= "libaio",
	.version		= FIO_IOOPS_VERSION,
	.init			= fio_libaio_init,
	.post_init		= fio_libaio_post_init,
	.io_u_init		= fio_libaio_io_u_init,
	.prep			= fio_libaio_prep,
	.queue			= fio_libaio_queue,
	.commit			= fio_libaio_commit,
	.cancel			= fio_libaio_cancel,
	.getevents		= fio_libaio_getevents,
	.event			= fio_libaio_event,
	.cleanup		= fio_libaio_cleanup,
	.open_file		= generic_open_file,
	.close_file		= generic_close_file,
	.get_file_size		= generic_get_file_size,
	.options		= options,
	.option_struct_size	= sizeof(struct libaio_options),
};

static void fio_init fio_libaio_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_libaio_unregister(void)
{
	unregister_ioengine(&ioengine);
}
