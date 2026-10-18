// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/ctype.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/tlv.h>
#include <sound/info.h>
#include <sound/initval.h>
#include "azcard.h"

MODULE_AUTHOR("audiolaz");
MODULE_DESCRIPTION("audiolaz: a sound card whose clock is the reader's (the player's DAC)");
MODULE_LICENSE("GPL");

#define AZC_MAX_ST 8

static int index = SNDRV_DEFAULT_IDX1;
static char *id = "audiolaz";
static char *name = "audiolaz";
static char *longname = "";
static char *usb_id = "";
static int streams = 4;
static int max_buffer_ms = 100;
static int max_period_ms = 25;
module_param(index, int, 0444);
MODULE_PARM_DESC(index, "Index value for the audiolaz card.");
module_param(id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for the audiolaz card.");
module_param(name, charp, 0444);
MODULE_PARM_DESC(name, "Its name as applications show it (the DAC's, so that they show the DAC behind).");
module_param(longname, charp, 0444);
MODULE_PARM_DESC(longname, "Its long name (the DAC's maker and model).");
module_param(usb_id, charp, 0444);
MODULE_PARM_DESC(usb_id, "The DAC's USB id VVVV:PPPP, shown as /proc/asound/cardN/usbid.");
module_param(streams, int, 0444);
MODULE_PARM_DESC(streams, "Playback streams, one per source (1 to 8).");
module_param(max_buffer_ms, int, 0644);
MODULE_PARM_DESC(max_buffer_ms, "An application's buffer at most (ms, 10 to 2000; 0: no cap), from its next stream on.");
module_param(max_period_ms, int, 0644);
MODULE_PARM_DESC(max_period_ms, "Its periods at most (ms, 2 to 500; 0: no cap), from its next stream on.");

#define AZC_VOL_MIN_CDB (-6000)    /* the volume control: −60 dB … 0 dB, steps of 0.5 dB, muted at its bottom */
#define AZC_VOL_STEP_CDB 50
#define AZC_VOL_STEPS ((-AZC_VOL_MIN_CDB) / AZC_VOL_STEP_CDB)
#define AZC_TICK_MS 5              /* the stand-in clock's tick */
#define AZC_BOUNCE (64 * 1024)     /* the reader's frames, copied out under the lock at most this much at a time */

static const unsigned int azc_rate_list[AZC_RATES_N] = {
	44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000,
};

struct azc;
struct azc_st {                     /* one playback stream (substream n) and its reader */
	struct azc *az;
	int n;
	spinlock_t lock;                /* everything below but the mutex's and the device's */
	struct snd_pcm_substream *ss;   /* while open */
	int running;
	u32 gen;
	u64 read;                       /* frames taken since prepare (by the reader or the stand-in clock) */
	u64 stand_in;                   /* of which the stand-in clock's */
	unsigned int rate, channels, frame_bytes, period, buffer;
	u32 fmt;
	u32 delay;                      /* frames heard after the reader (AZC_IOC_SET_DELAY) */
	u32 rates;                      /* AZC_RATE_* the next streams may take (0: all) */
	unsigned long last_read;        /* jiffies of the reader's last read (0: none yet) */
	unsigned long hold_until;       /* AZC_IOC_HOLD: no stand-in before (0: none) */
	ktime_t tick_t;                 /* the stand-in clock: when it last counted */
	u64 tick_frac;                  /* its frames × NSEC_PER_SEC not yet whole */
	wait_queue_head_t wq;
	struct mutex rd_mu;             /* one read at a time */
	char *bounce;                   /* AZC_BOUNCE bytes, the reader's (under rd_mu) */
	atomic_t readers;
	struct snd_pcm_hw_constraint_list rate_c;
	unsigned int rate_c_list[AZC_RATES_N];
	struct miscdevice misc;
	char dev[32];
	int misc_on;
};

struct azc {
	struct kref ref;                /* the list, the card, each reader's open file */
	struct list_head node;          /* azc_list */
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct platform_device *pdev;
	int is_out;                     /* an output's card (AZC_IOC_ADD_OUT), not the module's own */
	char usb[16];                   /* /proc/asound/card<N>/usbid ("" none) */
	char name0[32], lname0[80];     /* its names at its making (AZC_IOC_SET_NAME "": back to them) */
	int nst;
	struct azc_st st[AZC_MAX_ST];
	int vol;                        /* stream 0's volume: 0 … AZC_VOL_STEPS (full) */
	int sw;                         /* its switch: 1 on */
	struct delayed_work tick;
	struct mutex name_mu;           /* AZC_IOC_SET_NAME: one rename at a time */
};

static struct platform_device *azc_pdev_of(void);
static LIST_HEAD(azc_list);         /* every card: the module's own first, then the outputs' */
static DEFINE_MUTEX(azc_list_mu);
static void azc_az_free(struct kref *k);
static void azc_put(struct azc *az) { kref_put(&az->ref, azc_az_free); }

struct azc_reader {                 /* one per open of a reader's device */
	struct azc_st *st;
	u32 seen_gen;                   /* the stream's generation as AZC_IOC_INFO last told it */
};

static const struct snd_pcm_hardware azc_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_SYNC_APPLPTR,   /* every move of the application's pointer through the ack (mmap included) */
	.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_3LE |
		   SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_KNOT,
	.rate_min = 44100,
	.rate_max = 768000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 8 * 1024 * 1024,
	.period_bytes_min = 256,
	.period_bytes_max = 2 * 1024 * 1024,
	.periods_min = 2,
	.periods_max = 1024,
};

static u32 azc_fmt_of(snd_pcm_format_t f)
{
	switch (f) {
	case SNDRV_PCM_FORMAT_S16_LE: return AZC_FMT_S16;
	case SNDRV_PCM_FORMAT_S24_LE: return AZC_FMT_S24;
	case SNDRV_PCM_FORMAT_S24_3LE: return AZC_FMT_S24_3;
	case SNDRV_PCM_FORMAT_S32_LE: return AZC_FMT_S32;
	default: return 0;
	}
}

static struct azc_st *azc_st_of(struct snd_pcm_substream *ss)
{
	struct azc *az = snd_pcm_substream_chip(ss);

	return ss->number < az->nst ? &az->st[ss->number] : NULL;
}

/* frames the application wrote and nobody took yet (under st->lock) */
static snd_pcm_uframes_t azc_avail(struct azc_st *st)
{
	snd_pcm_uframes_t appl;
	u64 a;

	if (!st->ss || !st->running)
		return 0;
	appl = READ_ONCE(st->ss->runtime->control->appl_ptr);
	smp_rmb();                      /* the frames before appl_ptr, then */
	a = (u64)appl;                  /* appl_ptr wraps at rt->boundary: on 64 bits, after centuries */
	if (a <= st->read)
		return 0;
	/* one frame left while the application plays (all of it while it drains: its end): the reader taking the
	 * last one is an empty buffer to the core, now told of every move — an underrun, Roon preparing its stream again
	 * six times a second */
	if (st->ss->runtime->state != SNDRV_PCM_STATE_DRAINING)
		return a - st->read > 1 ? (snd_pcm_uframes_t)(a - st->read - 1) : 0;
	return (snd_pcm_uframes_t)(a - st->read);
}

/* frames taken: the pointer moves, the application told at each period crossed (outside st->lock): the core takes each
 * word as a whole period gone (17/10: told of every move, it counted a buffer more now and then — Roon preparing its
 * stream again every 1.8 s, crackles); the reader takes what there is, so that it crosses them (roomd, src_loop.c) */
static void azc_took(struct azc_st *st, struct snd_pcm_substream *ss, u64 before, u64 after)
{
	unsigned long fl;

	if (!ss)
		return;
	if (st->period && before / st->period != after / st->period)
		snd_pcm_period_elapsed(ss);

	if (READ_ONCE(ss->runtime->state) != SNDRV_PCM_STATE_DRAINING)
		return;
	snd_pcm_stream_lock_irqsave(ss, fl);
	if (ss->runtime && ss->runtime->state == SNDRV_PCM_STATE_DRAINING &&
	    (u64)READ_ONCE(ss->runtime->control->appl_ptr) <= after)
		snd_pcm_stop(ss, SNDRV_PCM_STATE_SETUP);
	snd_pcm_stream_unlock_irqrestore(ss, fl);
}

/* ---- the PCM ---- */
static int azc_open(struct snd_pcm_substream *ss)
{
	struct azc_st *st = azc_st_of(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	unsigned long fl;
	u32 rates;
	int i, n = 0;

	if (!st)
		return -ENODEV;
	rt->hw = azc_hw;
	spin_lock_irqsave(&st->lock, fl);
	rates = st->rates;
	spin_unlock_irqrestore(&st->lock, fl);
	for (i = 0; i < AZC_RATES_N; i++)
		if (!rates || (rates & (1u << i)))
			st->rate_c_list[n++] = azc_rate_list[i];
	if (!n)
		return -EINVAL;
	st->rate_c.count = n;
	st->rate_c.list = st->rate_c_list;
	st->rate_c.mask = 0;
	rt->hw.rate_min = st->rate_c_list[0];
	rt->hw.rate_max = st->rate_c_list[n - 1];
	i = snd_pcm_hw_constraint_list(rt, 0, SNDRV_PCM_HW_PARAM_RATE, &st->rate_c);
	if (i < 0)
		return i;
	/* the application's buffer and periods capped: what it writes ahead is heard that much later */
	{
		const int b = READ_ONCE(max_buffer_ms), p = READ_ONCE(max_period_ms);

		if (b > 0) {
			i = snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_BUFFER_TIME, 0,
							 1000u * (unsigned int)clamp(b, 10, 2000));
			if (i < 0)
				return i;
		}
		if (p > 0) {
			i = snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_PERIOD_TIME, 0,
							 1000u * (unsigned int)clamp(p, 2, 500));
			if (i < 0)
				return i;
		}
	}
	spin_lock_irqsave(&st->lock, fl);
	st->ss = ss;
	spin_unlock_irqrestore(&st->lock, fl);
	return 0;
}

static int azc_close(struct snd_pcm_substream *ss)
{
	struct azc_st *st = azc_st_of(ss);
	unsigned long fl;

	if (!st)
		return 0;
	spin_lock_irqsave(&st->lock, fl);
	st->ss = NULL;
	st->running = 0;
	st->gen++;
	spin_unlock_irqrestore(&st->lock, fl);
	wake_up_interruptible(&st->wq);
	return 0;
}

static int azc_prepare(struct snd_pcm_substream *ss)
{
	struct azc_st *st = azc_st_of(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	unsigned long fl;

	if (!st)
		return -ENODEV;
	spin_lock_irqsave(&st->lock, fl);
	st->rate = rt->rate;
	st->channels = rt->channels;
	st->frame_bytes = frames_to_bytes(rt, 1);
	st->period = rt->period_size;
	st->buffer = rt->buffer_size;
	st->fmt = azc_fmt_of(rt->format);
	st->read = 0;
	st->stand_in = 0;
	st->running = 0;
	st->gen++;
	rt->delay = st->delay;
	spin_unlock_irqrestore(&st->lock, fl);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)

	ss->wait_time = AZC_STALE_MS + AZC_HOLD_MAX_MS + 100 + 2 * (long)div_u64((u64)rt->buffer_size * 1000, rt->rate ? rt->rate : 1);   /* a hold too */
#endif
	wake_up_interruptible(&st->wq);
	return 0;
}

/* the buffer about to be freed (the core frees it after this): no read of it from now on (the lock waits for one going) */
static int azc_hw_free(struct snd_pcm_substream *ss)
{
	struct azc_st *st = azc_st_of(ss);
	unsigned long fl;

	if (!st)
		return 0;
	spin_lock_irqsave(&st->lock, fl);
	st->running = 0;
	st->buffer = 0;
	st->frame_bytes = 0;
	st->gen++;
	spin_unlock_irqrestore(&st->lock, fl);
	wake_up_interruptible(&st->wq);
	/* a read (or the stand-in clock's tick) still telling the core what it took, outside st->lock (azc_took):
	 * over before the runtime goes (the core frees it after hw_free and close) */
	mutex_lock(&st->rd_mu);
	mutex_unlock(&st->rd_mu);
	return 0;
}

/* the application wrote (or moved its pointer): the reader woken at once */
static int azc_ack(struct snd_pcm_substream *ss)
{
	struct azc_st *st = azc_st_of(ss);

	if (st)
		wake_up_interruptible(&st->wq);
	return 0;
}

static int azc_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct azc_st *st = azc_st_of(ss);

	if (!st)
		return -EINVAL;
	/* the stream's lock is held here: st->lock nests inside it */
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		spin_lock(&st->lock);
		st->running = 1;
		st->gen++;
		st->tick_t = ktime_get();
		st->tick_frac = 0;

		if (atomic_read(&st->readers))
			st->last_read = jiffies ? jiffies : 1;
		spin_unlock(&st->lock);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		spin_lock(&st->lock);
		st->running = 0;
		st->gen++;
		spin_unlock(&st->lock);
		break;
	default:
		return -EINVAL;
	}
	wake_up_interruptible(&st->wq);
	return 0;
}

static snd_pcm_uframes_t azc_pointer(struct snd_pcm_substream *ss)
{
	struct azc_st *st = azc_st_of(ss);
	snd_pcm_uframes_t p;

	if (!st)
		return 0;
	spin_lock(&st->lock);
	p = st->buffer ? (snd_pcm_uframes_t)(st->read % st->buffer) : 0;
	spin_unlock(&st->lock);
	return p;
}

static const struct snd_pcm_ops azc_ops = {
	.open = azc_open,
	.close = azc_close,
	.hw_free = azc_hw_free,
	.prepare = azc_prepare,
	.trigger = azc_trigger,
	.pointer = azc_pointer,
	.ack = azc_ack,
};

/* ---- the stand-in clock: frames let go at the nominal rate while nobody reads a stream that plays ---- */
static void azc_tick_st(struct azc_st *st, ktime_t now)
{
	struct snd_pcm_substream *ss = NULL;
	unsigned long fl;
	u64 before = 0, after = 0;

	if (!mutex_trylock(&st->rd_mu))  /* a reader reading: not needed; and hw_free waits for azc_took (rd_mu) */
		return;
	spin_lock_irqsave(&st->lock, fl);
	if (st->running && st->ss && st->rate &&
	    (!st->last_read || time_after(jiffies, st->last_read + msecs_to_jiffies(AZC_STALE_MS))) &&
	    !(st->hold_until && atomic_read(&st->readers) && time_before(jiffies, st->hold_until))) {
		u64 ns = ktime_to_ns(ktime_sub(now, st->tick_t));
		u64 want = ns * st->rate + st->tick_frac;
		u64 n = div64_u64(want, NSEC_PER_SEC);
		snd_pcm_uframes_t avail = azc_avail(st);

		st->tick_frac = want - n * NSEC_PER_SEC;
		if (n > avail)               /* never past what the application wrote */
			n = avail;
		if (st->buffer > 1 && n > st->buffer / 2)   /* half a buffer at most a tick: the core sees each move */
			n = st->buffer / 2;
		before = st->read;
		st->read += n;
		st->stand_in += n;
		after = st->read;
		ss = st->ss;
	}
	st->tick_t = now;
	spin_unlock_irqrestore(&st->lock, fl);
	if (ss)
		azc_took(st, ss, before, after);
	mutex_unlock(&st->rd_mu);
}

static void azc_tick(struct work_struct *w)
{
	struct azc *az = container_of(to_delayed_work(w), struct azc, tick);
	ktime_t now = ktime_get();
	int i;

	for (i = 0; i < az->nst; i++)
		azc_tick_st(&az->st[i], now);
	schedule_delayed_work(&az->tick, msecs_to_jiffies(AZC_TICK_MS));
}

/* ---- the readers: /dev/audiolaz (stream 0), /dev/audiolaz<n> ---- */
static int azc_rd_open(struct inode *in, struct file *f)
{
	struct azc_st *st = container_of(f->private_data, struct azc_st, misc);   /* misc_open sets it */
	struct azc_reader *r;
	unsigned long fl;

	if (atomic_inc_return(&st->readers) > 1) {
		atomic_dec(&st->readers);
		return -EBUSY;
	}
	/* the card held while its reader's file is open (misc_open runs under misc_mtx: an output's card being
	 * taken away has deregistered this device first, or waits for this open to end) */
	kref_get(&st->az->ref);
	if (!st->bounce)
		st->bounce = kmalloc(AZC_BOUNCE, GFP_KERNEL);
	r = st->bounce ? kzalloc(sizeof *r, GFP_KERNEL) : NULL;
	if (!r) {
		atomic_dec(&st->readers);
		azc_put(st->az);
		return -ENOMEM;
	}
	r->st = st;
	spin_lock_irqsave(&st->lock, fl);
	r->seen_gen = st->gen;
	spin_unlock_irqrestore(&st->lock, fl);
	f->private_data = r;
	return 0;
}

static int azc_rd_release(struct inode *in, struct file *f)
{
	struct azc_reader *r = f->private_data;
	struct azc_st *st = r->st;
	unsigned long fl;

	spin_lock_irqsave(&st->lock, fl);
	st->last_read = 0;              /* the stand-in clock at once */
	st->hold_until = 0;
	spin_unlock_irqrestore(&st->lock, fl);
	kfree(r);
	atomic_dec(&st->readers);
	azc_put(st->az);
	return 0;
}

static ssize_t azc_rd_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
	struct azc_st *st = ((struct azc_reader *)f->private_data)->st;
	struct snd_pcm_substream *ss;
	unsigned long fl;
	size_t done = 0;
	u32 gen;
	int e = 0;

	if (mutex_lock_interruptible(&st->rd_mu))
		return -ERESTARTSYS;
	spin_lock_irqsave(&st->lock, fl);
	gen = st->gen;
	spin_unlock_irqrestore(&st->lock, fl);
	while (done < count) {
		snd_pcm_uframes_t avail, n, at, first;
		unsigned int fb;
		u64 before, after;
		const char *src;

		spin_lock_irqsave(&st->lock, fl);
		st->last_read = jiffies ? jiffies : 1;
		st->hold_until = 0;             /* read: the hold over */
		fb = st->frame_bytes;
		if (st->gen != gen || !st->running || !st->ss || !fb || !st->buffer) {
			/* no stream playing (none yet, or it stopped or changed): 0 at once, never a wait for the next one (16/10:
			 * a read asked just as a stream ended waited for one that never came); poll's EPOLLPRI tells a new one */
			spin_unlock_irqrestore(&st->lock, fl);
			break;
		}
		avail = azc_avail(st);
		n = min_t(size_t, (count - done) / fb, AZC_BOUNCE / fb);
		if (!n) {
			spin_unlock_irqrestore(&st->lock, fl);
			break;
		}
		if (!avail) {
			spin_unlock_irqrestore(&st->lock, fl);
			if (done)
				break;
			if (f->f_flags & O_NONBLOCK) { e = -EAGAIN; break; }
			/* woken by the application's next write (ack), a stop, or 2 ms (the stand-in clock's view of us) */
			e = wait_event_interruptible_timeout(st->wq, READ_ONCE(st->gen) != gen, msecs_to_jiffies(2));
			if (e < 0) break;
			e = 0;
			continue;
		}
		if (n > avail)
			n = avail;
		/* half a buffer at most between two words to the core: a whole buffer at once brings the pointer back
		 * where it was (read % buffer), the core seeing no move, the buffer full for ever to the application */
		if (st->buffer > 1 && n > st->buffer / 2)
			n = st->buffer / 2;
		/* copied out under the lock: hw_free (the buffer then freed by the core) waits for it */
		ss = st->ss;
		src = ss->runtime->dma_area;
		at = (snd_pcm_uframes_t)(st->read % st->buffer);
		first = st->buffer - at;
		if (first > n)
			first = n;
		memcpy(st->bounce, src + (size_t)at * fb, (size_t)first * fb);
		if (n > first)
			memcpy(st->bounce + (size_t)first * fb, src, (size_t)(n - first) * fb);
		before = st->read;
		st->read += n;
		after = st->read;
		spin_unlock_irqrestore(&st->lock, fl);
		if (copy_to_user(buf + done, st->bounce, (size_t)n * fb)) {
			e = -EFAULT;
			break;
		}
		done += (size_t)n * fb;
		azc_took(st, ss, before, after);
	}
	mutex_unlock(&st->rd_mu);
	return done ? (ssize_t)done : e;
}

static __poll_t azc_rd_poll(struct file *f, poll_table *pt)
{
	struct azc_reader *r = f->private_data;
	struct azc_st *st = r->st;
	unsigned long fl;
	__poll_t m = 0;

	poll_wait(f, &st->wq, pt);
	spin_lock_irqsave(&st->lock, fl);
	if (azc_avail(st))
		m |= EPOLLIN | EPOLLRDNORM;
	if (st->gen != r->seen_gen)     /* a stream prepared, started, stopped or gone since AZC_IOC_INFO */
		m |= EPOLLPRI;
	spin_unlock_irqrestore(&st->lock, fl);
	return m;
}

static void azc_set_name(struct azc *az, const char *nm, const char *lnm);

static long azc_rd_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct azc_reader *r = f->private_data;
	struct azc_st *st = r->st;
	struct azc *az = st->az;
	unsigned long fl;

	switch (cmd) {
	case AZC_IOC_INFO: {
		struct azc_info i;
		int vol = READ_ONCE(az->vol), sw = READ_ONCE(az->sw);

		memset(&i, 0, sizeof i);
		spin_lock_irqsave(&st->lock, fl);
		i.gen = st->gen;
		i.running = st->running && st->ss;
		i.rate = st->rate;
		i.channels = st->channels;
		i.format = st->fmt;
		i.frame_bytes = st->frame_bytes;
		i.period = st->period;
		i.buffer = st->buffer;
		i.read = st->read;
		i.written = st->ss ? (u64)READ_ONCE(st->ss->runtime->control->appl_ptr) : 0;
		i.volume_cdb = st->n == 0 ? AZC_VOL_MIN_CDB + vol * AZC_VOL_STEP_CDB : 0;   /* the control: stream 0's */
		i.mute = st->n == 0 && (!sw || vol == 0);
		i.stand_in = (u32)st->stand_in;
		r->seen_gen = st->gen;
		spin_unlock_irqrestore(&st->lock, fl);
		return copy_to_user((void __user *)arg, &i, sizeof i) ? -EFAULT : 0;
	}
	case AZC_IOC_SET_DELAY: {
		u32 d;

		if (get_user(d, (u32 __user *)arg))
			return -EFAULT;
		spin_lock_irqsave(&st->lock, fl);
		st->delay = d;
		if (st->ss)
			st->ss->runtime->delay = d;
		spin_unlock_irqrestore(&st->lock, fl);
		return 0;
	}
	case AZC_IOC_HOLD: {            /* azcard.h */
		u32 ms;

		if (get_user(ms, (u32 __user *)arg))
			return -EFAULT;
		ms = min_t(u32, ms, AZC_HOLD_MAX_MS);
		spin_lock_irqsave(&st->lock, fl);
		st->hold_until = ms ? (jiffies + msecs_to_jiffies(ms)) | 1 : 0;
		spin_unlock_irqrestore(&st->lock, fl);
		return 0;
	}
	case AZC_IOC_SET_RATES: {
		u32 v;

		if (get_user(v, (u32 __user *)arg))
			return -EFAULT;
		spin_lock_irqsave(&st->lock, fl);
		st->rates = v & ((1u << AZC_RATES_N) - 1);
		spin_unlock_irqrestore(&st->lock, fl);
		return 0;
	}
	case AZC_IOC_SET_NAME: {
		struct azc_name nm;

		if (copy_from_user(&nm, (void __user *)arg, sizeof nm))
			return -EFAULT;
		nm.name[sizeof nm.name - 1] = 0;
		nm.longname[sizeof nm.longname - 1] = 0;
		azc_set_name(az, nm.name, nm.longname);
		return 0;
	}
	}
	return -ENOTTY;
}

static const struct file_operations azc_fops = {
	.owner = THIS_MODULE,
	.open = azc_rd_open,
	.release = azc_rd_release,
	.read = azc_rd_read,
	.poll = azc_rd_poll,
	.unlocked_ioctl = azc_rd_ioctl,
	.compat_ioctl = azc_rd_ioctl,
	.llseek = noop_llseek,
};

static const DECLARE_TLV_DB_SCALE(azc_tlv, AZC_VOL_MIN_CDB, AZC_VOL_STEP_CDB, 1);

static int azc_vol_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *u)
{
	u->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	u->count = 1;
	u->value.integer.min = 0;
	u->value.integer.max = AZC_VOL_STEPS;
	return 0;
}

static int azc_vol_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *u)
{
	struct azc *az = snd_kcontrol_chip(k);

	u->value.integer.value[0] = READ_ONCE(az->vol);
	return 0;
}

static int azc_vol_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *u)
{
	struct azc *az = snd_kcontrol_chip(k);
	long v = u->value.integer.value[0];
	int changed;

	if (v < 0 || v > AZC_VOL_STEPS)
		return -EINVAL;
	changed = READ_ONCE(az->vol) != v;
	WRITE_ONCE(az->vol, (int)v);
	return changed;
}

static int azc_sw_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *u)
{
	struct azc *az = snd_kcontrol_chip(k);

	u->value.integer.value[0] = READ_ONCE(az->sw);
	return 0;
}

static int azc_sw_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *u)
{
	struct azc *az = snd_kcontrol_chip(k);
	int v = !!u->value.integer.value[0], changed = READ_ONCE(az->sw) != v;

	WRITE_ONCE(az->sw, v);
	return changed;
}

static const struct snd_kcontrol_new azc_ctls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "PCM Playback Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.info = azc_vol_info,
		.get = azc_vol_get,
		.put = azc_vol_put,
		.tlv = { .p = azc_tlv },
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "PCM Playback Switch",
		.info = snd_ctl_boolean_mono_info,
		.get = azc_sw_get,
		.put = azc_sw_put,
	},
};

static void azc_name_put(char *d, size_t n, const char *s)
{
	char t[80];

	memset(t, 0, sizeof t);
	strscpy(t, s, min(n, sizeof t));
	memcpy(d, t, min(n, sizeof t));   /* the whole field: what was after the new name's end zeroed too */
}

static void azc_set_name(struct azc *az, const char *nm, const char *lnm)
{
	struct snd_card *card = az->card;
	char l[80];

	mutex_lock(&az->name_mu);
	if (!*nm) {
		nm = az->name0;
		lnm = az->lname0;
	}
	if (*lnm)
		strscpy(l, lnm, sizeof l);
	else if (az->is_out && az->lname0[0])
		strscpy(l, az->lname0, sizeof l);   /* an output's card: its DAC's long name kept (Roon's) */
	else if (*nm && nm != az->name0)
		strscpy(l, nm, sizeof l);
	else
		snprintf(l, sizeof l, "%s (the player's DAC clock)", az->name0);
	azc_name_put(card->shortname, sizeof card->shortname, nm);
	azc_name_put(card->longname, sizeof card->longname, l);
	azc_name_put(az->pcm->name, sizeof az->pcm->name, nm);
	mutex_unlock(&az->name_mu);
}

/* /proc/asound/card<N>/usbid, as a USB card has it: the DAC behind (usb_id=) */
static void azc_proc_usbid(struct snd_info_entry *e, struct snd_info_buffer *b)
{
	struct azc *az = e->private_data;

	snd_iprintf(b, "%s\n", az->usb);
}

/* (17/10, diagnosis) /proc/asound/card<N>/azstate: each stream as the card holds it — open, running, its generation,
 * frames read (by the reader or the stand-in clock) and written, the stand-in's share, the reader's last read, readers */
static void azc_proc_state(struct snd_info_entry *e, struct snd_info_buffer *b)
{
	struct azc *az = e->private_data;
	int i;

	for (i = 0; i < az->nst; i++) {
		struct azc_st *st = &az->st[i];
		unsigned long fl, lr;
		u64 rd, si, appl = 0;
		int on, run, state = -1;
		u32 gen;

		spin_lock_irqsave(&st->lock, fl);
		on = !!st->ss; run = st->running; gen = st->gen; rd = st->read; si = st->stand_in; lr = st->last_read;
		if (st->ss) { appl = (u64)READ_ONCE(st->ss->runtime->control->appl_ptr); state = (int)st->ss->runtime->state; }
		spin_unlock_irqrestore(&st->lock, fl);
		snd_iprintf(b, "%d: open %d state %d running %d gen %u read %llu written %llu stand_in %llu last_read %ld ms ago readers %d\n",
			    i, on, state, run, gen, rd, appl, si, lr ? (long)jiffies_to_msecs(jiffies - lr) : -1L, atomic_read(&st->readers));
	}
}

/* ---- the card ---- */
static void azc_misc_off(struct azc *az)
{
	int i;

	for (i = 0; i < az->nst; i++)
		if (az->st[i].misc_on) {
			misc_deregister(&az->st[i].misc);
			az->st[i].misc_on = 0;
		}
}

/* the last reference gone (the card freed, every reader's file closed): its memory */
static void azc_az_free(struct kref *k)
{
	struct azc *az = container_of(k, struct azc, ref);
	int i;

	for (i = 0; i < az->nst; i++)
		kfree(az->st[i].bounce);
	kfree(az);
}

static void azc_card_free(struct snd_card *card)
{
	struct azc *az = card->private_data;

	if (az)
		azc_put(az);
}

#define AZC_MAX_OUTS 8             /* the outputs' cards at most */

static struct azc *azc_make(struct platform_device *pdev, int idx, const char *cid, const char *nm, const char *lnm,
			    const char *usb, int nst, const char *dev0, int is_out)
{
	struct snd_card *card;
	struct azc *az;
	int e, i;

	if (is_out) {
		for (i = 0; i < SNDRV_CARDS; i++) {
			struct snd_card *c = snd_card_ref(i);
			int same = c && !strcmp(c->id, cid);

			if (c)
				snd_card_unref(c);
			if (same)
				return ERR_PTR(-EEXIST);
		}
	}
	az = kzalloc(sizeof *az, GFP_KERNEL);
	if (!az)
		return ERR_PTR(-ENOMEM);
	kref_init(&az->ref);
	INIT_LIST_HEAD(&az->node);
	e = snd_card_new(&pdev->dev, idx, cid, THIS_MODULE, 0, &card);
	if (e < 0) {
		kfree(az);
		return ERR_PTR(e);
	}
	card->private_data = az;
	card->private_free = azc_card_free;    /* from here on, the card's reference: freed with it */
	az->card = card;
	az->pdev = pdev;
	az->is_out = is_out;
	strscpy(az->usb, usb ? usb : "", sizeof az->usb);
	strscpy(az->name0, nm, sizeof az->name0);
	strscpy(az->lname0, lnm ? lnm : "", sizeof az->lname0);
	az->nst = nst < 1 ? 1 : nst > AZC_MAX_ST ? AZC_MAX_ST : nst;
	az->vol = AZC_VOL_STEPS;
	az->sw = 1;
	for (i = 0; i < az->nst; i++) {
		struct azc_st *st = &az->st[i];

		st->az = az;
		st->n = i;
		spin_lock_init(&st->lock);
		init_waitqueue_head(&st->wq);
		mutex_init(&st->rd_mu);
		atomic_set(&st->readers, 0);
		if (i)
			snprintf(st->dev, sizeof st->dev, "%s%d", dev0, i);
		else
			strscpy(st->dev, dev0, sizeof st->dev);
		st->misc.minor = MISC_DYNAMIC_MINOR;
		st->misc.name = st->dev;
		st->misc.fops = &azc_fops;
		st->misc.mode = 0660;
	}
	INIT_DELAYED_WORK(&az->tick, azc_tick);
	mutex_init(&az->name_mu);
	strscpy(card->driver, "audiolaz", sizeof card->driver);
	strscpy(card->shortname, nm, sizeof card->shortname);
	if (lnm && *lnm)
		strscpy(card->longname, lnm, sizeof card->longname);
	else
		snprintf(card->longname, sizeof card->longname, "%s (the player's DAC clock)", nm);
	e = snd_pcm_new(card, nm, 0, az->nst, 0, &az->pcm);
	if (e < 0)
		goto fail;
	az->pcm->private_data = az;
	strscpy(az->pcm->name, nm, sizeof az->pcm->name);
	snd_pcm_set_ops(az->pcm, SNDRV_PCM_STREAM_PLAYBACK, &azc_ops);
	snd_pcm_set_managed_buffer_all(az->pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);
	for (i = 0; i < ARRAY_SIZE(azc_ctls); i++) {
		e = snd_ctl_add(card, snd_ctl_new1(&azc_ctls[i], az));
		if (e < 0)
			goto fail;
	}
	if (az->usb[0]) {
		e = snd_card_ro_proc_new(card, "usbid", az, azc_proc_usbid);
		if (e < 0)
			goto fail;
	}
	e = snd_card_ro_proc_new(card, "azstate", az, azc_proc_state);
	if (e < 0)
		goto fail;
	e = snd_card_register(card);
	if (e < 0)
		goto fail;
	if (is_out && strcmp(card->id, cid)) {   /* made meanwhile by another under that id */
		e = -EEXIST;
		goto fail;
	}
	for (i = 0; i < az->nst; i++) {
		e = misc_register(&az->st[i].misc);
		if (e < 0) {
			azc_misc_off(az);
			goto fail;
		}
		az->st[i].misc_on = 1;
	}
	schedule_delayed_work(&az->tick, msecs_to_jiffies(AZC_TICK_MS));
	return az;
fail:
	snd_card_free(card);                   /* the azc with it (azc_card_free) */
	return ERR_PTR(e);
}

/* A card taken away: no new reader, its clock stopped, gone to the applications as a USB card unplugged (an open
 * stream ends with -ENODEV), freed once the last one closes it; its readers' files keep the azc until closed. */
static void azc_unmake(struct azc *az, int wait)
{
	azc_misc_off(az);
	cancel_delayed_work_sync(&az->tick);
	if (wait)
		snd_card_free(az->card);       /* the module's own, as before: waits for its files */
	else
		snd_card_free_when_closed(az->card);
}

static int azc_probe(struct platform_device *pdev)
{
	struct azc *az = azc_make(pdev, index, id, name, longname, usb_id, streams, "audiolaz", 0);

	if (IS_ERR(az))
		return PTR_ERR(az);
	mutex_lock(&azc_list_mu);
	list_add(&az->node, &azc_list);
	mutex_unlock(&azc_list_mu);
	platform_set_drvdata(pdev, az);
	return 0;
}

static void azc_remove(struct platform_device *pdev)
{
	struct azc *az, *t;
	LIST_HEAD(gone);

	mutex_lock(&azc_list_mu);
	list_splice_init(&azc_list, &gone);
	mutex_unlock(&azc_list_mu);
	list_for_each_entry_safe(az, t, &gone, node) {
		list_del_init(&az->node);
		azc_unmake(az, !az->is_out);
	}
	platform_set_drvdata(pdev, NULL);
}

/* ---- /dev/audiolaz-ctl: the outputs' cards ---- */

static int azc_id_ok(const char *s)
{
	size_t n = strnlen(s, 16), i;

	if (n < 1 || n > 15 || !isalpha(s[0]))
		return 0;
	for (i = 0; i < n; i++)
		if (!isalnum(s[i]) && s[i] != '_')
			return 0;
	return 1;
}

static int azc_usb_ok(const char *s)
{
	size_t i;

	if (!*s)
		return 1;
	if (strnlen(s, 16) != 9 || s[4] != ':')
		return 0;
	for (i = 0; i < 9; i++)
		if (i != 4 && !isxdigit(s[i]))
			return 0;
	return 1;
}

static struct azc *azc_find(const char *cid)   /* under azc_list_mu */
{
	struct azc *az;

	list_for_each_entry(az, &azc_list, node)
		if (!strcmp(az->card->id, cid))
			return az;
	return NULL;
}

static long azc_ctl_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct azc_out o;
	struct azc *az;
	int n = 0;
	long e = 0;

	if (cmd != AZC_IOC_ADD_OUT && cmd != AZC_IOC_DEL_OUT)
		return -ENOTTY;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (copy_from_user(&o, (void __user *)arg, sizeof o))
		return -EFAULT;
	o.id[sizeof o.id - 1] = 0;
	o.name[sizeof o.name - 1] = 0;
	o.longname[sizeof o.longname - 1] = 0;
	o.usb_id[sizeof o.usb_id - 1] = 0;
	if (!azc_id_ok(o.id) || !azc_usb_ok(o.usb_id))
		return -EINVAL;
	mutex_lock(&azc_list_mu);
	az = azc_find(o.id);
	if (cmd == AZC_IOC_DEL_OUT) {
		if (!az || !az->is_out)
			e = -ENOENT;
		else
			list_del_init(&az->node);
		mutex_unlock(&azc_list_mu);
		if (!e)
			azc_unmake(az, 0);
		return e;
	}
	if (az && !az->is_out) {
		e = -EEXIST;                   /* the module's own card's id: never an output's */
	} else if (az) {
		o.flags = 0;                   /* there already: as it is (its name the player's to change) */
	} else {
		char dev[32];

		list_for_each_entry(az, &azc_list, node)
			n += az->is_out;
		if (n >= AZC_MAX_OUTS || !azc_pdev_of()) {
			e = n >= AZC_MAX_OUTS ? -ENOSPC : -ENODEV;
		} else {
			snprintf(dev, sizeof dev, "audiolaz-%s", o.id);
			az = azc_make(azc_pdev_of(), SNDRV_DEFAULT_IDX1, o.id, o.name[0] ? o.name : o.id, o.longname, o.usb_id, 1, dev, 1);
			if (IS_ERR(az)) {
				e = PTR_ERR(az);
			} else {
				list_add_tail(&az->node, &azc_list);
				o.flags = 1;
			}
		}
	}
	if (!e) {
		o.card = az->card->number;
		snprintf(o.dev, sizeof o.dev, "/dev/%s", az->st[0].dev);
	}
	mutex_unlock(&azc_list_mu);
	if (e)
		return e;
	return copy_to_user((void __user *)arg, &o, sizeof o) ? -EFAULT : 0;
}

static const struct file_operations azc_ctl_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = azc_ctl_ioctl,
	.compat_ioctl = azc_ctl_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice azc_ctl = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "audiolaz-ctl",
	.fops = &azc_ctl_fops,
	.mode = 0600,
};
static int azc_ctl_on;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
static int azc_remove_int(struct platform_device *pdev) { azc_remove(pdev); return 0; }
#endif

static struct platform_driver azc_driver = {
	.probe = azc_probe,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	.remove = azc_remove_int,
#else
	.remove = azc_remove,
#endif
	.driver = { .name = "snd_audiolaz" },
};

static struct platform_device *azc_pdev;
/* the device the outputs' cards hang from: the module's own card's, while it is bound (under azc_list_mu) */
static struct platform_device *azc_pdev_of(void)
{
	return azc_pdev && !IS_ERR(azc_pdev) && platform_get_drvdata(azc_pdev) ? azc_pdev : NULL;
}

static int __init azc_init(void)
{
	int e = platform_driver_register(&azc_driver);

	if (e < 0)
		return e;
	azc_pdev = platform_device_register_simple("snd_audiolaz", 0, NULL, 0);
	if (IS_ERR(azc_pdev)) {
		platform_driver_unregister(&azc_driver);
		return PTR_ERR(azc_pdev);
	}
	if (!platform_get_drvdata(azc_pdev)) {   /* the probe failed */
		platform_device_unregister(azc_pdev);
		platform_driver_unregister(&azc_driver);
		return -ENODEV;
	}
	/* the outputs' cards asked there; a module without it (refused): the module's own card alone, as before */
	azc_ctl_on = misc_register(&azc_ctl) == 0;
	return 0;
}

static void __exit azc_exit(void)
{
	if (azc_ctl_on)
		misc_deregister(&azc_ctl);
	platform_device_unregister(azc_pdev);
	platform_driver_unregister(&azc_driver);
}

module_init(azc_init);
module_exit(azc_exit);
