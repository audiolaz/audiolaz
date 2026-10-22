/* The system's own inputs (11/10, the UPnP input): src_loop.c (Linux's Loopback) and src_pipe.c (macOS's pipes) define
 * src.h's calls under the names plat_src_*, and src_upnp.c puts the player's built-in inputs (the UPnP/DLNA renderer,
 * upnp.c) beside them: it is src.h's side roomd.c calls, each input k handed to its own. Included by those two files
 * before src.h (their names renamed), and by src_upnp.c (SRC_PLAT_DECLARE: the plat_ declarations, then the names
 * given back). */
#ifndef SRC_PLAT_UNDO
#define src_init plat_src_init
#define src_what plat_src_what
#define src_active plat_src_active
#define src_rate plat_src_rate
#define src_channels plat_src_channels
#define src_relayout plat_src_relayout
#define src_started plat_src_started
#define src_closed plat_src_closed
#define src_paced plat_src_paced
#define src_io_ended plat_src_io_ended
#define src_keeps plat_src_keeps
#define src_event_fds plat_src_event_fds
#define src_drain plat_src_drain
#define src_shift plat_src_shift
#define src_air_connected plat_src_air_connected
#define src_owns plat_src_owns
#define src_stop plat_src_stop
#define src_air_meta plat_src_air_meta
#define src_air_item plat_src_air_item
#define src_air_name plat_src_air_name
#define src_status plat_src_status
#define src_option plat_src_option
#define src_tick plat_src_tick
#define src_shutdown plat_src_shutdown
#define src_open plat_src_open
#define src_start plat_src_start
#define src_avail plat_src_avail
#define src_read plat_src_read
#define src_delay plat_src_delay
#define src_recover plat_src_recover
#define src_poll_fds plat_src_poll_fds
#define src_close plat_src_close
#define src_strerror plat_src_strerror
#define src_pcm plat_src_pcm
/* the built-in inputs' own calls (src.h), not the platform's: never renamed */
#else
#undef src_init
#undef src_what
#undef src_active
#undef src_rate
#undef src_channels
#undef src_relayout
#undef src_started
#undef src_closed
#undef src_paced
#undef src_io_ended
#undef src_keeps
#undef src_event_fds
#undef src_drain
#undef src_shift
#undef src_air_connected
#undef src_owns
#undef src_stop
#undef src_air_meta
#undef src_air_item
#undef src_air_name
#undef src_status
#undef src_option
#undef src_tick
#undef src_shutdown
#undef src_open
#undef src_start
#undef src_avail
#undef src_read
#undef src_delay
#undef src_recover
#undef src_poll_fds
#undef src_close
#undef src_strerror
#undef src_pcm
#endif
