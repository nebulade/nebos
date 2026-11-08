/* Syscall interface (int 0x80), shared by the kernel and ring-3 commands. */
#ifndef SYSCALL_H
#define SYSCALL_H

#define SYS_PUTSTR       1
#define SYS_GET_TICKS_MS 2
#define SYS_SLEEP_MS     3
#define SYS_EXIT         4

/* File service (see sys_fs.c).  Paths are absolute VFS paths; buffers live in
 * the calling process's address space (the kernel runs on its CR3, so no copy
 * in/out is needed). */
#define SYS_READ_FILE    5   /* (const char *path, void *buf, unsigned int max)
				-> bytes read, or <0 (SYS_E*) */
#define SYS_WRITE_FILE   6   /* (const char *path, const void *buf, unsigned int len)
				-> 0, or <0 */
#define SYS_LIST_DIR     7   /* (const char *path, unsigned int index,
				struct sys_dirent *out) -> 1 entry, 0 end, <0 */
#define SYS_UNLINK       8   /* (const char *path) -> 0, or <0 */
#define SYS_MKDIR        9   /* (const char *path) -> 0, or <0 */
#define SYS_RMDIR       10   /* (const char *path) -> 0, or <0 */
#define SYS_LIST_MOUNTS 11   /* (unsigned int index, struct sys_mount *out)
				-> 1 mount, 0 end */

/* Kernel-state info for the info commands (see sys_info.c). */
#define SYS_MEMINFO      12  /* (struct sys_meminfo *out) -> 0 */
#define SYS_E820         13  /* (unsigned int index, struct sys_e820 *out)-> 1, 0 */
#define SYS_MOUSEINFO    14  /* (struct sys_mouseinfo *out) -> 1 when present */
#define SYS_STORAGE_INFO 15  /* (struct sys_storage_info *out) -> 0;
				refreshes the mount cache like `storage` did */
#define SYS_BLKDEV       16  /* (unsigned int index, struct sys_blkdev *out) */

/* Network service (see sys_net.c): the stack and NIC drivers stay in the
 * kernel; user commands get the configuration, the DNS/DHCP clients and the
 * raw frame paths (so the packet-level commands port unchanged). */
#define SYS_NET_INFO      17  /* (struct sys_netinfo *out) -> 1 = NIC present */
#define SYS_NET_PRINT_INFO 18 /* the driver's status lines */
#define SYS_NET_ACQUIRE   19  /* take the receiver for this process (0/-1) */
#define SYS_NET_RELEASE   20
#define SYS_NET_MAC       21  /* (unsigned char mac[6]) -> 0/-1 */
#define SYS_NET_GET_IP    22  /* (unsigned char ip[4]) */
#define SYS_NET_SET_IP    23  /* (const unsigned char ip[4]) */
#define SYS_NET_GET_MASK  24
#define SYS_NET_GET_GW    25
#define SYS_NET_GET_DNS   26
#define SYS_NET_SET_DNS   27
#define SYS_NET_TX        28  /* (const void *buf, unsigned len) -> 0/-1 */
#define SYS_NET_RX        29  /* (void *buf, unsigned max) -> len/-1 */
#define SYS_NET_DECODE    30  /* (const void *buf, unsigned len): one line */
#define SYS_NET_REPLY     31  /* (in, len, out, max) -> reply length / 0 */
#define SYS_NET_LOOPBACK  32  /* (int on) -> 0/-1 */
#define SYS_NET_REGS      33  /* the driver's register dump */
#define SYS_NET_RESET     34  /* driver re-init */
#define SYS_DHCP_RUN      35  /* (timeout_ms) -> dhcp_run() result */
#define SYS_DHCP_INFO     36  /* (unsigned char ip[4], unsigned *lease) -> 1 */
#define SYS_DNS_RESOLVE   37  /* (name, unsigned char ip[4]) -> 0; caller owns */
#define SYS_NETLOG        38  /* (const char *text) -> 0/-1 */
#define SYS_NETLOG_CONF   39  /* (int enabled_or_-1, const ip[4] or NULL) */

/* TCP client (see sys_net.c/tcp.c): one connection at a time; the caller
 * must own the NIC (SYS_NET_ACQUIRE). */
#define SYS_TCP_CONNECT   40  /* (mac[6], ip[4], port) -> 0/-1/-2/-3 */
#define SYS_TCP_PEER_MSS  41  /* () -> the peer's MSS of the current conn */
#define SYS_TCP_SEND      42  /* (buf, len) -> 0/-1/-2/-3 */
#define SYS_TCP_RECV      43  /* (buf, max, timeout_ms) -> >0/0/-1/-2 */
#define SYS_TCP_CLOSE     44  /* () */
#define SYS_NET_NEXT_HOP  45  /* (dst[4], hop[4]) */
#define SYS_NET_ARP       46  /* (ip[4], mac[6]) -> 0/-1/-2 (net_arp_resolve) */
#define SYS_NET_CONFIGURED 47 /* () -> 1 when an IP address is set */
#define SYS_GETCWD       48  /* (char *buf, unsigned max) -> 0; the caller's cwd */

/* Process snapshot for the `top` command (see process.c). */
#define SYS_PROCINFO     49  /* (unsigned int index, struct sys_procinfo *out)
				-> 1 when found, 0 at the end of the table */
#define SYS_KEY_POLL     55  /* () -> 0 none, ASCII, or IO_KEY_* for the caller's window */
#define SYS_RTC_HMS      56  /* (struct sys_time *out) -> 0/-1 */
#define SYS_RTC_GET     104  /* (struct sys_datetime *out) -> 0/-1 */
#define SYS_RTC_SET     105  /* (const struct sys_datetime *in) -> 0/-1 */
#define SYS_NTP_SYNC    106  /* (const char *server, unsigned timeout_ms,
				struct sys_datetime *out) -> SYS_NTP_*;
				the service owns the NIC for the duration */

/* SYS_NTP_SYNC results. */
#define SYS_NTP_OK       0
#define SYS_NTP_ENONIC  (-1)  /* no NIC */
#define SYS_NTP_ENAME   (-2)  /* server name could not be resolved */
#define SYS_NTP_EARP    (-3)  /* no ARP reply for the next hop */
#define SYS_NTP_ETX     (-4)  /* NIC transmit failed */
#define SYS_NTP_ETIMEOUT (-5) /* no NTP reply */
#define SYS_NTP_EREPLY  (-6)  /* malformed, unsynchronised or KoD reply */
#define SYS_NTP_EBUSY   (-7)  /* another process owns the receiver */

/* Window-manager service (see wm_abi.h).  Only the kernel-spawned WM process
 * may call the WM_* entries; ordinary commands get -1. */
#define SYS_WM_ATTACH      57  /* (struct wm_screen_info *out) -> 0/-1 */
#define SYS_WM_INFO        58  /* (index, struct wm_window_info *out) -> 1/0/-1 */
#define SYS_WM_MAP         59  /* (index, struct wm_map_info *out) -> 0/-1 */
#define SYS_WM_ACK         60  /* (index, dirty_seq) -> 0/-1 */
#define SYS_WM_FOCUS       61  /* (index) -> 0/-1 */
#define SYS_WM_SET_GEOM    62  /* (index, struct wm_geom *in) -> 0/-1 */
#define SYS_WM_CREATE_TERM 63  /* () -> index / -1 */
#define SYS_WM_CLOSE       64  /* (index) -> 0/-1 */
#define SYS_WM_POLL        65  /* (struct wm_status *out) -> 0/-1 */
#define SYS_WM_BEGIN       66  /* () -> 0/-1; erase cursor, frame active */
#define SYS_WM_PRESENT     67  /* (struct wm_present_info *in) -> 0/-1 */
#define SYS_WM_SYNC        68  /* () -> 0/-1; run deferred gfx cleanup */
#define SYS_RTC_DATETIME   69  /* (char *buf, unsigned max) -> 0/-1 */
#define SYS_WIN_CREATE     70  /* (struct win_create *in, struct win_info *out) */
#define SYS_WIN_PRESENT    71  /* (const struct wm_win_present *in) -> 0/-1;
				  NULL or w/h 0 publishes the whole canvas */
#define SYS_WIN_CLOSE      72  /* () -> 0/-1; close the caller's window */
#define SYS_WM_RESIZE      83  /* (index, struct wm_size *content) -> 0/-1 */
#define SYS_WIN_INFO       84  /* (struct win_info *out) -> 0/-1 */
#define SYS_WIN_ACK_RESIZE 85  /* () -> 0/-1; swap in the pending canvas */
#define SYS_WIN_POINTER    86  /* (struct win_pointer *out) -> 0/-1 */
#define SYS_WIN_EVENT      87  /* (struct win_event *out) -> 1 event, 0 none */

/* Per-process anonymous heap (see sys_mem.c).  One growable arena at
 * USER_HEAP_BASE (0x60000000), at most USER_HEAP_MAX (32 MB), freed when the
 * process exits.  SYS_MEM_ALLOC returns the base address, or <0.  Ring-3
 * commands use malloc/free (libc/malloc.c); these syscalls are the libc
 * backend (libc/ucmd_mem.c) and are not called by application code. */
#define SYS_MEM_ALLOC      88  /* (unsigned bytes) -> VA or <0 */
#define SYS_MEM_GROW       89  /* (unsigned total_bytes) -> 0/-1; new pages not zeroed */
#define SYS_MEM_FREE       90  /* () -> 0/-1 */

/* CPU temperature (see thermal.c): the hottest core's DTS reading. */
#define SYS_CPU_TEMP       91  /* (struct sys_cputemp *out) -> 0/-1 */

/* IT87xx Super I/O hardware monitor (see it87.c); read-only. */
#define SYS_SENSORS        92  /* (struct sys_sensors *out) -> 0/-1 */

/* TCP server (WebDAV listener).  The caller must own the NIC (SYS_NET_ACQUIRE).
 * SYS_TCP_ACCEPT polls a few frames and returns 1 once a connection is ready,
 * 0 when nothing arrived, or -3 when a netkexec header was captured (the
 * command should exit and let the kworker take over). */
#define SYS_TCP_LISTEN     93  /* (port) -> 0/-1 */
#define SYS_TCP_ACCEPT     94  /* () -> 1/0/-2/-3 */
#define SYS_TCP_SRV_RECV   95  /* (buf, max, timeout_ms) -> >0/0/-1/-2 */
#define SYS_TCP_SRV_SEND   96  /* (buf, len) -> 0/-1/-2/-3 */
#define SYS_TCP_SRV_CLOSE  97
#define SYS_TCP_PEER       99  /* (ip[4], unsigned *port) -> 0/-1 (debug log) */
#define SYS_NET_SPEED     100  /* (mbps) -> 0; 0 = auto, 100 = force 100M */
#define SYS_NET_MAC_SET   101  /* (const mac[6]) -> 0/-1/-2 */

/* ThinkPad embedded-controller sensors (see tpec.c); read-only. */
#define SYS_TPEC_SENSORS  102  /* (struct sys_tpec_sensors *out) -> 0/-1 */

/* AC/battery snapshot from the board EC (see power_src.c, tpec.c and
 * cros_ec.c).  SYS_POWER_F_DETAIL also reads design capacity, the capacity
 * unit and the manufacturer/model/serial strings. */
#define SYS_POWER_INFO     103  /* (struct sys_powerinfo *out, flags) -> 0/-1 */
#define SYS_POWER_F_DETAIL 0x1

#define SYS_POWER_SRC_NONE 0
#define SYS_POWER_SRC_TPEC 1   /* ThinkPad H8 EC */
#define SYS_POWER_SRC_CROS 2   /* Chrome OS EC */

/* Chrome OS EC temperature/fan/switch snapshot (see cros_ec.c). */
#define SYS_CROS_EC_SENSORS 107  /* (struct sys_cros_sensors *out) -> 0/-1 */

/* Stat one path (the file service has no open handle). */
#define SYS_FS_STAT        98  /* (path, struct sys_stat *out) -> 0/<0 */

/* Terminal / process services used by the ring-3 shell. */
#define SYS_TTY_CREATE     73  /* () -> tty id 1..TTY_MAX / -1 */
#define SYS_TTY_READ       74  /* (id, buf, max) -> bytes / 0 / -1 */
#define SYS_SPAWN          75  /* (path, arg, tty, cwd) -> pid / <0 */
#define SYS_SPAWN_KCMD     76  /* (name, arg, tty) -> pid / <0 */
#define SYS_ALIVE          77  /* (pid) -> 1/0 */
#define SYS_KILL           78  /* (pid) -> 0/-1 */
#define SYS_CHDIR          79  /* (path) -> 0/-1 */
#define SYS_WIN_KEY_WAIT   80  /* (timeout_ms) -> key / 0 timeout / -1 */
#define SYS_SCHEDINFO      81  /* (struct sys_schedinfo *out) -> 0/-1 */

struct sys_spawn {
	const char *path;
	const char *arg;
	const char *cwd;
	unsigned int tty;
};

struct sys_netinfo {
	unsigned int present;
	unsigned char mac[6];
	unsigned char ip[4];
	unsigned char mask[4];
	unsigned char gw[4];
	unsigned char dns[4];
	unsigned int mirror_on;
	unsigned char mirror_host[4];
	char name[8];
	char chip[16];
};

/* Error codes returned by the file syscalls. */
#define SYS_EIO     (-1)     /* disk I/O error */
#define SYS_ENODEV  (-2)     /* no such device / unusable path prefix */
#define SYS_ENOFS   (-3)     /* device exists but has no mounted filesystem */
#define SYS_ENOENT  (-4)     /* file/directory not found */
#define SYS_EINVAL  (-5)     /* path is not valid for this operation */
#define SYS_EWRITE  (-6)     /* write refused (full, or FAT rejected it) */
#define SYS_EROFS   (-7)     /* read-only filesystem (ramfs) */
#define SYS_ENOSPC  (-8)     /* no space left on the filesystem */

/* One directory entry.  `name` is a display name: raw 8.3 for FAT mounts
 * (space padded, `raw_83` set) or the plain name for name-string filesystems
 * such as ramfs and /share. */
struct sys_dirent {
	char name[64];
	unsigned int size;
	unsigned int is_dir;
	unsigned int raw_83;
};

/* One mounted filesystem (for `ls /`). */
struct sys_mount {
	char name[16];
	char type[8];           /* "FAT12", "FAT16", "FAT32", "ramfs", "share" */
	unsigned int total_sectors;
	char label[12];
	unsigned int ro;
};

/* Result of SYS_FS_STAT. */
struct sys_stat {
	unsigned int size;
	unsigned int is_dir;
	unsigned int ro;
};

struct sys_meminfo {
	unsigned int total_kb;
	unsigned int free_pages;
	unsigned int page_size;
	unsigned int e820_count;
};

/* One live process, for `top` (PROC_NAME_LEN = 12).  cpu_ticks/life_ticks
 * are PIT ticks (1 kHz), so lifecycle percentages can be derived. */
struct sys_procinfo {
	unsigned int pid;
	unsigned int state;       /* PROC_*: 1 ready, 2 running, 4 sleep, 5 block */
	unsigned int cpu_ticks;   /* ticks spent as the running process */
	unsigned int life_ticks;  /* ticks since creation, >= 1 */
	unsigned int pages;       /* resident 4 KB pages (stack + dir + user) */
	unsigned int weight;      /* CFS weight, 1..SCHED_WEIGHT_MAX */
	unsigned int cpu;         /* CPU id, or 0xFFFFFFFF when not running */
	char name[12];
};

/* Scheduler counters for `top` and the `sched` command. */
struct sys_schedinfo {
	unsigned int ncpus;
	unsigned int runnable;
	unsigned int sleeping;
	unsigned int blocked;
	unsigned int zombies;
	unsigned int switches;
	unsigned int wakeups;
	unsigned int idle_ticks;
	unsigned int slice_ticks;
};

/* CPU temperature snapshot (see thermal.c).  temp_c is the hottest core's
 * last DTS sample; a negative value means no Intel DTS is available.  delta_c
 * and status are from the CPU that serviced the syscall (for diagnostics).
 * cpu_temp_c[] holds the per-logical-CPU cached samples. */
#define SYS_CPU_MAX 8
struct sys_cputemp {
	int temp_c;
	int tjmax_c;
	int delta_c;
	unsigned int status;
	int cpu_temp_c[SYS_CPU_MAX];
	unsigned int ncpus;
};

/* IT87xx hardware monitor snapshot (temps are signed; -128 = unconnected). */
struct sys_sensors {
	unsigned int present;
	unsigned int chip_id;
	unsigned int base;
	unsigned int temp_enable;
	unsigned int temp_extra;
	int temp_c[3];
	unsigned int fan_rpm[3];
};

/* ThinkPad embedded-controller snapshot (see tpec.c).  temp_c[] follows
 * thinkpad_acpi's temp1..temp16 (EC 0x78..0x7F, then 0xC0..0xC7); -128 means
 * the sensor is not present.  fan_level is the raw EC 0x2F value. */
#define SYS_TPEC_TEMP_MAX 16
struct sys_tpec_sensors {
	unsigned int present;
	char vendor[16];
	char product[32];
	char version[32];
	int temp_c[SYS_TPEC_TEMP_MAX];
	unsigned int fan_rpm;
	unsigned int fan_level;
};

/* AC/battery snapshot.  rate is the signed present rate in the same unit as
 * the capacities; unit_mwh (when unit_known) selects mWh/mW vs mAh/mA.
 * percent is valid only when percent_valid is set.  cycle_count/serial are
 * zero/empty when the backend does not provide them. */
#define SYS_POWER_BATT_MAX 2
struct sys_battery {
	unsigned int present;
	unsigned int error;
	unsigned int status;      /* raw EC status/flags byte */
	unsigned int charging;
	unsigned int discharging;
	unsigned int percent;
	unsigned int percent_valid;
	unsigned int remaining;
	unsigned int full;
	unsigned int design;
	unsigned int voltage_mv;
	int rate;
	unsigned int unit_known;
	unsigned int unit_mwh;
	char manufacturer[16];
	char model[16];
	unsigned int cycle_count;
	char serial[16];
};

struct sys_powerinfo {
	unsigned int present;     /* a supported EC was found */
	unsigned int source;      /* SYS_POWER_SRC_* */
	int ac_online;            /* -1 unknown, 0 offline, 1 online */
	unsigned int ec_rev;
	char vendor[16];
	char product[32];
	char version[32];
	unsigned int battery_count;
	struct sys_battery battery[SYS_POWER_BATT_MAX];
};

/* Chrome OS EC board sensors (see cros_ec.c).  temp_c[] is -128 when the
 * sensor is absent/unavailable; fan_present is a bitmask over fan_rpm[].
 * switches follows EC_MEMMAP_SWITCHES (bit 0 lid open, bit 1 power button). */
#define SYS_CROS_TEMP_MAX 24
#define SYS_CROS_FAN_MAX 4
struct sys_cros_sensors {
	unsigned int present;
	unsigned int acpi_version;
	unsigned int battery_version;
	unsigned int thermal_version;
	int temp_c[SYS_CROS_TEMP_MAX];
	unsigned int fan_rpm[SYS_CROS_FAN_MAX];
	unsigned int fan_present;
	unsigned int fan_duty;     /* 0xff = automatic */
	unsigned int switches;
};

/* Wall clock for the ring-3 clock (see rtc.c). */
struct sys_time {
	unsigned int hour;     /* 0-23 */
	unsigned int minute;   /* 0-59 */
	unsigned int second;   /* 0-59 */
};

/* Full RTC wall clock for `date` and the Date & Time app.  The CMOS RTC
 * stores a two-digit year; the kernel expands it with a 1970-2069 window
 * (70-99 -> 1900s, 00-69 -> 2000s), so a dead-battery default such as 1980
 * or 1990 reads correctly.  SYS_RTC_SET accepts the same range. */
struct sys_datetime {
	unsigned int year;     /* 1970-2069 */
	unsigned int month;    /* 1-12 */
	unsigned int day;      /* 1-31 */
	unsigned int hour;     /* 0-23 */
	unsigned int minute;   /* 0-59 */
	unsigned int second;   /* 0-59 */
	unsigned int wday;     /* 0 = Sunday */
};

struct sys_e820 {
	unsigned int base;      /* low 32 bits, as meminfo always printed */
	unsigned int length;
	unsigned int type;
};

struct sys_mouseinfo {
	unsigned int present;
	unsigned int ps2;
	unsigned int ps2_id;
	unsigned int ps2_packet_size;
	unsigned int ps2_streaming;
	unsigned int usb;
	unsigned int usb_ctrl;
	unsigned int usb_addr;
	unsigned int usb_ep;
	unsigned int usb_mps;
	unsigned int usb_low;
	unsigned int usb_keyboard;
	unsigned int cursor_visible;
	char hcd[8];
	char backend[16];
	unsigned int touchpad;
	unsigned int tp_vendor;
	unsigned int tp_product;
};

struct sys_storage_info {
	unsigned int ahci_present;
	unsigned int blkdev_count;
};

struct sys_blkdev {
	char name[8];
	char model[41];
	unsigned int kind;       /* 0 = floppy, 1 = IDE, 2 = SATA */
	unsigned int present;
	unsigned int total_sectors;
};

int syscall_dispatch(int num, int a1, int a2, int a3);

#endif
