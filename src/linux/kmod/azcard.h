// SPDX-License-Identifier: GPL-2.0 OR MIT
#ifndef AUDIOLAZ_AZCARD_H
#define AUDIOLAZ_AZCARD_H
#include <linux/types.h>          /* __u32, __s32, __u64: the kernel's own, in both (Linux only) */
#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

#define AZC_DEV "/dev/audiolaz"
#define AZC_STALE_MS 200           /* no read this long while the application plays: the card's own clock */

/* the stream's sample format, as the reader gets it */
#define AZC_FMT_S16 1              /* S16_LE */
#define AZC_FMT_S24 2              /* S24_LE: 24 bits in the low three bytes of 32 */
#define AZC_FMT_S24_3 3            /* S24_3LE: three bytes */
#define AZC_FMT_S32 4              /* S32_LE */

/* the rates (AZC_IOC_SET_RATES) */
#define AZC_RATE_44100   (1u << 0)
#define AZC_RATE_48000   (1u << 1)
#define AZC_RATE_88200   (1u << 2)
#define AZC_RATE_96000   (1u << 3)
#define AZC_RATE_176400  (1u << 4)
#define AZC_RATE_192000  (1u << 5)
#define AZC_RATE_352800  (1u << 6)
#define AZC_RATE_384000  (1u << 7)
#define AZC_RATE_705600  (1u << 8)
#define AZC_RATE_768000  (1u << 9)
#define AZC_RATES_N 10

struct azc_info {
	__u32 gen;                 /* +1 at each stream the application prepares */
	__u32 running;             /* 1 while the application plays (started, not paused nor stopped) */
	__u32 rate, channels, format, frame_bytes;   /* format: AZC_FMT_*, 0 before any stream */
	__u32 period, buffer;      /* the application's, in frames */
	__u64 read;                /* frames the reader took since the stream's start */
	__u64 written;             /* frames the application wrote since the stream's start */
	__s32 volume_cdb;
	__u32 mute;                /* its switch off */
	__u32 stand_in;            /* frames the card's own clock let go unread (no reader), this stream */
	__u32 reserved[5];
};

#define AZC_IOC_MAGIC 'z'
#define AZC_IOC_INFO       _IOR(AZC_IOC_MAGIC, 1, struct azc_info)
#define AZC_IOC_SET_DELAY  _IOW(AZC_IOC_MAGIC, 2, __u32)
#define AZC_IOC_SET_RATES  _IOW(AZC_IOC_MAGIC, 3, __u32)

struct azc_name {                  /* AZC_IOC_SET_NAME: UTF-8, NUL-terminated (cut to the card's own fields: 32, 80) */
	char name[32];
	char longname[80];         /* card->longname ("": the name) */
};
#define AZC_IOC_SET_NAME   _IOW(AZC_IOC_MAGIC, 4, struct azc_name)

/* AZC_IOC_HOLD: the reader opens its output (a network renderer: a second or more): the card's own clock does
 * not stand in for it for up to that many ms (AZC_HOLD_MAX_MS at most) or until its next read; the application's
 * writes wait meanwhile, as for a DAC that starts. 0: no more. */
#define AZC_HOLD_MAX_MS 4000
#define AZC_IOC_HOLD       _IOW(AZC_IOC_MAGIC, 7, __u32)

#define AZC_CTL_DEV "/dev/audiolaz-ctl"
struct azc_out {                   /* AZC_IOC_ADD_OUT, AZC_IOC_DEL_OUT (id alone) */
	char id[16];
	char name[32];
	char longname[80];         /* "" the name */
	char usb_id[16];           /* "" none, or VVVV:PPPP */
	__s32 card;                /* out: its index (/proc/asound/card<N>) */
	__u32 flags;               /* out: 1 made now, 0 there already */
	char dev[32];              /* out: its reader's device */
	__u32 reserved[4];
};
#define AZC_IOC_ADD_OUT    _IOWR(AZC_IOC_MAGIC, 5, struct azc_out)
#define AZC_IOC_DEL_OUT    _IOW(AZC_IOC_MAGIC, 6, struct azc_out)

#endif
