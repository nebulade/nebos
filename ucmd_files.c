/* files: a small graphical file manager built on the ngui toolkit.
 *
 * The window is a canvas window (SYS_WIN_CREATE), so it owns its pixels and
 * receives keys and pointer events.  It browses the VFS: "/" lists the mounted
 * filesystems, everything else lists a directory through SYS_LIST_DIR. */

#include "ucmd_lib.h"
#include "ngui.h"
#include "img.h"

#define FM_PATH_MAX     128
#define FM_MAX_ENTRIES  256
#define FM_MAX_MOUNTS   8
#define FM_ROW_H        22
#define FM_TOOLBAR_H    30
#define FM_PATH_H       26
#define FM_STATUS_H     18
#define FM_SIDEBAR_W    140
#define FM_SCROLL_W     9
#define FM_EDIT_MAX     16384
#define FM_STATUS_MS    3000

enum {
	ID_UP = 1,
	ID_HOME,
	ID_REFRESH,
	ID_NEW,
	ID_GO,
	ID_PATH,
	ID_NEW_INPUT,
	ID_CREATE,
	ID_CANCEL,
};

#define ACT_UP       (1u << 0)
#define ACT_HOME     (1u << 1)
#define ACT_REFRESH  (1u << 2)
#define ACT_NEW      (1u << 3)
#define ACT_NAV      (1u << 4)
#define ACT_LOAD     (1u << 5)
#define ACT_OPEN     (1u << 6)
#define ACT_PARENT   (1u << 7)
#define ACT_CREATE   (1u << 8)
#define ACT_CANCEL   (1u << 9)
#define ACT_CLOSE    (1u << 10)

struct fm_entry {
	char name[64];
	char detail[8];
	unsigned int size;
	unsigned int is_dir;
	unsigned int is_mount;
};

static struct fm_entry entries[FM_MAX_ENTRIES];
static struct fm_entry load_entries[FM_MAX_ENTRIES];
static unsigned int entry_count;
static int truncated;
static int selected;
static char cwd[FM_PATH_MAX];
static char action_path[FM_PATH_MAX];

static struct sys_mount mounts[FM_MAX_MOUNTS];
static unsigned int mount_count;

static struct ngui ui;
static struct ngui_input path_in;
static struct ngui_input new_in;
static struct ngui_scroll list_scroll;
static int modal;
static int last_click_index = -1;
static unsigned int last_click_ms;

static char status_text[96];
static unsigned int status_until;
static int status_error;

/* ---- tiny freestanding string helpers ---- */

static unsigned int s_len(const char *s)
{
	unsigned int n = 0;

	while (s && s[n])
		n++;
	return n;
}

static void s_copy(char *d, const char *s, unsigned int cap)
{
	unsigned int i = 0;

	if (!cap)
		return;
	while (s && s[i] && i + 1 < cap) {
		d[i] = s[i];
		i++;
	}
	d[i] = '\0';
}

static void s_append(char *d, unsigned int *n, const char *s, unsigned int cap)
{
	while (s && *s && *n + 1 < cap)
		d[(*n)++] = *s++;
	d[*n] = '\0';
}

static void s_append_uint(char *d, unsigned int *n, unsigned int v,
			  unsigned int cap)
{
	char tmp[12];
	unsigned int i = 0, j;

	do {
		tmp[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i < sizeof(tmp));
	for (j = 0; j < i && *n + 1 < cap; j++)
		d[(*n)++] = tmp[i - 1 - j];
	d[*n] = '\0';
}

static int s_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a != *b)
			return 0;
		a++;
		b++;
	}
	return *a == *b;
}

static int s_ci_cmp(const char *a, const char *b)
{
	for (;;) {
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;

		if (ca >= 'a' && ca <= 'z')
			ca = (unsigned char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (unsigned char)(cb - 32);
		if (ca != cb)
			return (int)ca - (int)cb;
		if (!ca)
			return 0;
	}
}

static int is_root(const char *p)
{
	return p && p[0] == '/' && p[1] == '\0';
}

static int ext_is(const char *name, const char *ext)
{
	unsigned int n = s_len(name), e = s_len(ext), i;

	if (n < e)
		return 0;
	for (i = 0; i < e; i++) {
		unsigned char a = (unsigned char)name[n - e + i];
		unsigned char b = (unsigned char)ext[i];

		if (a >= 'A' && a <= 'Z')
			a = (unsigned char)(a + 32);
		if (a != b)
			return 0;
	}
	return 1;
}

static int is_image_name(const char *name)
{
	return ext_is(name, ".png") || ext_is(name, ".jpg") ||
	       ext_is(name, ".jpeg") || ext_is(name, ".gif") ||
	       ext_is(name, ".bmp") || ext_is(name, ".tga") ||
	       ext_is(name, ".ppm") || ext_is(name, ".pgm") ||
	       ext_is(name, ".pbm") || ext_is(name, ".ico") ||
	       ext_is(name, ".cur") || ext_is(name, ".pam");
}

static void path_join(const char *dir, const char *name, char *out,
		      unsigned int cap)
{
	unsigned int n = 0;

	if (!dir || !dir[0])
		dir = "/";
	if (name && name[0] == '/') {
		s_copy(out, name, cap);
		return;
	}
	s_append(out, &n, dir, cap);
	if (!is_root(dir))
		s_append(out, &n, "/", cap);
	s_append(out, &n, name, cap);
}

static void path_parent(const char *in, char *out, unsigned int cap)
{
	unsigned int n, i, last = 0;

	s_copy(out, in, cap);
	if (!out[0] || is_root(out)) {
		s_copy(out, "/", cap);
		return;
	}
	n = s_len(out);
	if (n > 1 && out[n - 1] == '/')
		out[--n] = '\0';
	for (i = 0; i < n; i++)
		if (out[i] == '/')
			last = i;
	if (last == 0)
		out[1] = '\0';
	else
		out[last] = '\0';
	if (!out[0])
		s_copy(out, "/", cap);
}

static void make_abs(const char *p, char *out, unsigned int cap)
{
	if (!p || !p[0]) {
		s_copy(out, cwd, cap);
		return;
	}
	if (p[0] == '/')
		s_copy(out, p, cap);
	else
		path_join(cwd, p, out, cap);
}

static void format_name(const char *raw, unsigned int raw_83, char *out,
			unsigned int cap)
{
	/* Name-string filesystems (ramfs, share) already give a display name;
	 * FAT gives the raw 11-byte, space-padded 8.3 field. */
	if (!raw_83)
		s_copy(out, raw, cap);
	else
		u_fat_name(raw, out, cap);
}

static void format_size(unsigned int bytes, char *out, unsigned int cap)
{
	unsigned int n = 0;

	if (bytes < 1024) {
		s_append_uint(out, &n, bytes, cap);
		s_append(out, &n, " B", cap);
	} else if (bytes < 1024u * 1024u) {
		s_append_uint(out, &n, bytes / 1024, cap);
		s_append(out, &n, " KB", cap);
	} else {
		s_append_uint(out, &n, bytes / (1024u * 1024u), cap);
		s_append(out, &n, " MB", cap);
	}
}

/* ---- state / filesystem loading ---- */

static void set_status(const char *text, int error)
{
	s_copy(status_text, text, sizeof(status_text));
	status_until = u_ticks_ms() + FM_STATUS_MS;
	status_error = error;
	if (!status_until)
		status_until = 1;
	ngui_invalidate(&ui);
}

static void clear_status(void)
{
	status_text[0] = '\0';
	status_until = 0;
	status_error = 0;
}

static void load_mounts(void)
{
	struct sys_mount m;
	unsigned int i;

	mount_count = 0;
	for (i = 0; i < FM_MAX_MOUNTS; i++) {
		if (u_syscall2(SYS_LIST_MOUNTS, (int)i, (int)&m) != 1)
			break;
		mounts[mount_count++] = m;
	}
}

static int entry_before(const struct fm_entry *a, const struct fm_entry *b)
{
	int adir = (int)(a->is_dir || a->is_mount);
	int bdir = (int)(b->is_dir || b->is_mount);

	if (adir != bdir)
		return adir > bdir;
	return s_ci_cmp(a->name, b->name) < 0;
}

static void sort_entries(struct fm_entry *list, unsigned int n)
{
	unsigned int i, j;

	for (i = 1; i < n; i++) {
		struct fm_entry key = list[i];

		j = i;
		while (j > 0 && entry_before(&key, &list[j - 1])) {
			list[j] = list[j - 1];
			j--;
		}
		list[j] = key;
	}
}

static void commit_list(unsigned int n, const char *path)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		entries[i] = load_entries[i];
	entry_count = n;
	s_copy(cwd, path, sizeof(cwd));
	selected = 0;
	list_scroll.offset = 0;
	list_scroll.content_h = (int)(n * FM_ROW_H);
	last_click_index = -1;
	ngui_input_set(&path_in, cwd);
	ngui_invalidate(&ui);
}

static int load_root(void)
{
	unsigned int i;

	load_mounts();
	for (i = 0; i < mount_count; i++) {
		s_copy(load_entries[i].name, mounts[i].name,
		       sizeof(load_entries[i].name));
		s_copy(load_entries[i].detail, mounts[i].type,
		       sizeof(load_entries[i].detail));
		load_entries[i].size = 0;
		load_entries[i].is_dir = 1;
		load_entries[i].is_mount = 1;
	}
	sort_entries(load_entries, mount_count);
	truncated = 0;
	commit_list(mount_count, "/");
	clear_status();
	return 0;
}

static void list_error(int r)
{
	switch (r) {
	case SYS_ENOENT:
		set_status("No such file or directory", 1);
		break;
	case SYS_ENODEV:
		set_status("No such device", 1);
		break;
	case SYS_ENOFS:
		set_status("No filesystem on device", 1);
		break;
	case SYS_EINVAL:
		set_status("Not a directory", 1);
		break;
	case SYS_EIO:
		set_status("Disk read error", 1);
		break;
	default:
		set_status("Cannot open directory", 1);
		break;
	}
}

static int load_path(const char *path)
{
	struct sys_dirent d;
	unsigned int n = 0, i;
	int r;

	if (!path || !path[0] || is_root(path))
		return load_root();

	truncated = 0;
	for (i = 0; ; i++) {
		r = u_syscall3(SYS_LIST_DIR, (int)path, (int)i, (int)&d);
		if (r != 1) {
			if (r < 0) {
				list_error(r);
				return -1;
			}
			break;
		}
		if (d.name[0] == '.')
			continue;
		if (n >= FM_MAX_ENTRIES) {
			truncated = 1;
			break;
		}
		format_name(d.name, d.raw_83, load_entries[n].name,
			    sizeof(load_entries[n].name));
		load_entries[n].detail[0] = '\0';
		load_entries[n].size = d.size;
		load_entries[n].is_dir = d.is_dir;
		load_entries[n].is_mount = 0;
		n++;
	}
	sort_entries(load_entries, n);
	commit_list(n, path);
	clear_status();
	if (truncated)
		set_status("Showing the first 256 entries", 0);
	return 0;
}

static void navigate(const char *path)
{
	char abs[FM_PATH_MAX];

	if (!path || !path[0]) {
		set_status("Empty path", 1);
		return;
	}
	make_abs(path, abs, sizeof(abs));
	if (load_path(abs) == 0)
		ngui_set_focus(&ui, 0);
}

static void go_parent(void)
{
	char parent[FM_PATH_MAX];

	path_parent(cwd, parent, sizeof(parent));
	if (s_eq(parent, cwd)) {
		set_status("Already at the root", 0);
		return;
	}
	load_path(parent);
}

static void go_home(void)
{
	if (!is_root(cwd))
		load_path("/");
}

static void select_index(unsigned int idx)
{
	if (idx >= entry_count)
		return;
	selected = (int)idx;
	ngui_scroll_ensure_visible(&list_scroll, (int)idx * FM_ROW_H, FM_ROW_H);
	ngui_invalidate(&ui);
}

static void select_by_name(const char *name)
{
	unsigned int i;

	for (i = 0; i < entry_count; i++)
		if (s_ci_cmp(entries[i].name, name) == 0) {
			select_index(i);
			return;
		}
}

static void refresh(void)
{
	char keep[16];
	int had = 0;

	if (entry_count && selected >= 0 && selected < (int)entry_count) {
		s_copy(keep, entries[selected].name, sizeof(keep));
		had = 1;
	}
	if (load_path(cwd) != 0)
		return;
	if (had)
		select_by_name(keep);
}

static void open_selected(void)
{
	struct fm_entry *e;
	char path[FM_PATH_MAX];
	int pid;

	if (!entry_count || selected < 0 || selected >= (int)entry_count)
		return;
	e = &entries[selected];
	if (e->is_mount || e->is_dir) {
		if (e->is_mount)
			path_join("/", e->name, path, sizeof(path));
		else
			path_join(cwd, e->name, path, sizeof(path));
		load_path(path);
		return;
	}
	if (is_image_name(e->name)) {
		if (e->size > IMG_FILE_MAX) {
			set_status("Image is larger than 4 MB", 1);
			return;
		}
		path_join(cwd, e->name, path, sizeof(path));
		pid = u_spawn("/ramfs/bin/view", path, 0, cwd);
		if (pid <= 0)
			set_status("Cannot launch the viewer", 1);
		else
			set_status("Opened in the viewer", 0);
		return;
	}
	if (e->size > FM_EDIT_MAX) {
		set_status("File too large for the editor", 1);
		return;
	}
	path_join(cwd, e->name, path, sizeof(path));
	pid = u_spawn("/ramfs/bin/edit", path, 0, cwd);
	if (pid <= 0)
		set_status("Cannot launch the editor", 1);
	else
		set_status("Opened in the editor", 0);
}

static void new_folder(void)
{
	if (is_root(cwd)) {
		set_status("Select a filesystem first", 1);
		return;
	}
	modal = 1;
	ngui_input_set(&new_in, "");
	new_in.placeholder = "NAME";
	ngui_set_focus(&ui, ID_NEW_INPUT);
	ngui_invalidate(&ui);
}

static void create_folder(void)
{
	char path[FM_PATH_MAX];
	char made[16];
	int r;

	if (!new_in.text[0]) {
		set_status("Enter a folder name", 1);
		return;
	}
	path_join(cwd, new_in.text, path, sizeof(path));
	r = u_syscall1(SYS_MKDIR, (int)path);
	if (r != 0) {
		switch (r) {
		case SYS_EROFS:
			set_status("The filesystem is read-only", 1);
			break;
		case SYS_EINVAL:
			set_status("Invalid folder name", 1);
			break;
		case SYS_EIO:
			set_status("Disk write error", 1);
			break;
		default:
			set_status("Cannot create the folder", 1);
			break;
		}
		return;
	}
	s_copy(made, new_in.text, sizeof(made));
	modal = 0;
	ngui_set_focus(&ui, 0);
	refresh();
	select_by_name(made);
	clear_status();
	set_status("Folder created", 0);
}

static void do_action(unsigned int action)
{
	if (action & ACT_LOAD)
		load_path(action_path);
	if (action & ACT_NAV)
		navigate(path_in.text);
	if (action & ACT_UP)
		go_parent();
	if (action & ACT_PARENT)
		go_parent();
	if (action & ACT_HOME)
		go_home();
	if (action & ACT_REFRESH)
		refresh();
	if (action & ACT_NEW)
		new_folder();
	if (action & ACT_OPEN)
		open_selected();
	if (action & ACT_CREATE) {
		create_folder();
		if (modal)
			ngui_set_focus(&ui, ID_NEW_INPUT);
	}
	if (action & ACT_CANCEL) {
		modal = 0;
		ngui_set_focus(&ui, 0);
		ngui_invalidate(&ui);
	}
}

/* ---- drawing ---- */

static void draw_toolbar(struct ngui *g, struct ngui_rect r, unsigned int *action)
{
	int compact = g->w < 480;
	int y = r.y + 4;
	int h = 22;
	int x = r.x + 6;

	ngui_fill_rect(g, r, g->theme.panel);
	ngui_fill_rect(g, (struct ngui_rect){r.x, r.y + r.h - 1, r.w, 1},
		       g->theme.border);

	if (compact) {
		if (ngui_icon_button(g, ID_UP,
				     (struct ngui_rect){x, y, 30, h},
				     NGUI_ICON_UP))
			*action |= ACT_UP;
		x += 36;
		if (ngui_icon_button(g, ID_HOME,
				     (struct ngui_rect){x, y, 30, h},
				     NGUI_ICON_HOME))
			*action |= ACT_HOME;
		x += 36;
		if (ngui_icon_button(g, ID_REFRESH,
				     (struct ngui_rect){x, y, 30, h},
				     NGUI_ICON_REFRESH))
			*action |= ACT_REFRESH;
		x += 36;
		if (ngui_icon_button(g, ID_NEW,
				     (struct ngui_rect){x, y, 30, h},
				     NGUI_ICON_PLUS))
			*action |= ACT_NEW;
	} else {
		int w;

		w = (int)ngui_text_width("Up") + 18;
		if (ngui_button(g, ID_UP, (struct ngui_rect){x, y, w, h}, "Up"))
			*action |= ACT_UP;
		x += w + 6;
		w = (int)ngui_text_width("Home") + 18;
		if (ngui_button(g, ID_HOME, (struct ngui_rect){x, y, w, h},
				"Home"))
			*action |= ACT_HOME;
		x += w + 6;
		w = (int)ngui_text_width("Refresh") + 18;
		if (ngui_button(g, ID_REFRESH, (struct ngui_rect){x, y, w, h},
				"Refresh"))
			*action |= ACT_REFRESH;
		x += w + 6;
		w = (int)ngui_text_width("New Folder") + 18;
		if (ngui_button(g, ID_NEW, (struct ngui_rect){x, y, w, h},
				"New Folder"))
			*action |= ACT_NEW;
	}
}

static void draw_path(struct ngui *g, struct ngui_rect r, unsigned int *action)
{
	int go_w = g->w < 260 ? 40 : 50;
	struct ngui_rect in = {r.x + 6, r.y + 3, r.w - 12 - go_w - 6, 20};
	struct ngui_rect go = {r.x + r.w - 6 - go_w, r.y + 3, go_w, 20};
	int res;

	ngui_fill_rect(g, r, g->theme.panel);
	if (in.w < 40)
		in.w = 40;
	res = ngui_text_input(g, ID_PATH, in, &path_in);
	if (res & NGUI_INPUT_SUBMIT)
		*action |= ACT_NAV;
	if (res & NGUI_INPUT_CANCEL)
		ngui_set_focus(g, 0);
	if (ngui_button(g, ID_GO, go, "Go"))
		*action |= ACT_NAV;
}

static int path_under(const char *path, const char *base)
{
	unsigned int n = s_len(base);

	if (!path || !base)
		return 0;
	if (s_eq(path, base))
		return 1;
	if (s_len(path) > n && path[n] == '/' &&
	    path[0] == base[0]) {
		unsigned int i;

		for (i = 0; i < n; i++)
			if (path[i] != base[i])
				return 0;
		return 1;
	}
	return 0;
}

static void draw_sidebar(struct ngui *g, struct ngui_rect r, unsigned int *action)
{
	int y = r.y + 4;
	int row_y = r.y + 24;
	unsigned int i;

	ngui_fill_rect(g, r, g->theme.panel);
	ngui_text(g, r.x + 8, y, "Places", g->theme.text_dim);

	{
		int hot = g->inside && g->mouse_x >= r.x &&
			g->mouse_x < r.x + r.w &&
			g->mouse_y >= row_y && g->mouse_y < row_y + FM_ROW_H;
		int pressed = g->pressed && g->press_x >= r.x &&
			g->press_x < r.x + r.w &&
			g->press_y >= row_y && g->press_y < row_y + FM_ROW_H;

		if (is_root(cwd))
			ngui_fill_rect(g, (struct ngui_rect){r.x + 1, row_y,
						       r.w - 2, FM_ROW_H - 1},
				       g->theme.selection);
		else if (hot)
			ngui_fill_rect(g, (struct ngui_rect){r.x + 1, row_y,
						       r.w - 2, FM_ROW_H - 1},
				       g->theme.panel_alt);
		ngui_icon(g, (struct ngui_rect){r.x + 6, row_y + 3, 16, 16},
			  NGUI_ICON_HOME, g->theme.icon_folder);
		ngui_text(g, r.x + 28, row_y + 3, "Root", g->theme.text);
		if (pressed) {
			s_copy(action_path, "/", sizeof(action_path));
			*action |= ACT_LOAD;
		}
	}
	row_y += FM_ROW_H;

	for (i = 0; i < mount_count && row_y + FM_ROW_H <= r.y + r.h; i++) {
		char mpath[FM_PATH_MAX];
		int hot;
		int active;

		int pressed;

		path_join("/", mounts[i].name, mpath, sizeof(mpath));
		hot = g->inside && g->mouse_x >= r.x &&
			g->mouse_x < r.x + r.w &&
			g->mouse_y >= row_y && g->mouse_y < row_y + FM_ROW_H;
		pressed = g->pressed && g->press_x >= r.x &&
			g->press_x < r.x + r.w &&
			g->press_y >= row_y && g->press_y < row_y + FM_ROW_H;
		active = path_under(cwd, mpath);
		if (active)
			ngui_fill_rect(g, (struct ngui_rect){r.x + 1, row_y,
						       r.w - 2, FM_ROW_H - 1},
				       g->theme.selection);
		else if (hot)
			ngui_fill_rect(g, (struct ngui_rect){r.x + 1, row_y,
						       r.w - 2, FM_ROW_H - 1},
				       g->theme.panel_alt);
		ngui_icon(g, (struct ngui_rect){r.x + 6, row_y + 3, 16, 16},
			  NGUI_ICON_DISK, g->theme.icon_disk);
		ngui_text_ellipsis(g, (struct ngui_rect){r.x + 28, row_y,
					 r.w - 34, FM_ROW_H},
				   r.x + 28, row_y + 3, mounts[i].name,
				   g->theme.text);
		if (pressed) {
			s_copy(action_path, mpath, sizeof(action_path));
			*action |= ACT_LOAD;
		}
		row_y += FM_ROW_H;
	}
	if (!mount_count)
		ngui_text(g, r.x + 8, r.y + 50, "No volumes", g->theme.text_dim);
	ngui_fill_rect(g, (struct ngui_rect){r.x + r.w - 1, r.y, 1, r.h},
		       g->theme.border);
}

static void draw_list(struct ngui *g, struct ngui_rect r, unsigned int *action)
{
	unsigned int i;
	int scrolls;
	int row_w;
	int hover_idx = -1;
	int press_idx = -1;

	ngui_fill_rect(g, r, g->theme.bg);
	list_scroll.content_h = (int)(entry_count * FM_ROW_H);
	ngui_scroll_begin(g, r, &list_scroll);
	scrolls = list_scroll.content_h > list_scroll.view_h;
	row_w = r.w - (scrolls ? FM_SCROLL_W : 0);

	if (g->inside && g->mouse_x >= r.x && g->mouse_x < r.x + row_w &&
	    g->mouse_y >= r.y && g->mouse_y < r.y + r.h &&
	    list_scroll.content_h > 0) {
		int hi = (g->mouse_y - r.y + list_scroll.offset) / FM_ROW_H;

		if (hi >= 0 && hi < (int)entry_count)
			hover_idx = hi;
	}
	if (g->pressed && g->press_x >= r.x &&
	    g->press_x < r.x + row_w && g->press_y >= r.y &&
	    g->press_y < r.y + r.h) {
		press_idx = (g->press_y - r.y + list_scroll.offset) / FM_ROW_H;
		if (press_idx < 0 || press_idx >= (int)entry_count)
			press_idx = -1;
	}

	for (i = 0; i < entry_count; i++) {
		struct fm_entry *e = &entries[i];
		int y = r.y + (int)i * FM_ROW_H - list_scroll.offset;
		int detail_x;
		char detail[20];
		unsigned int dn = 0;

		if (y + FM_ROW_H <= r.y || y >= r.y + r.h)
			continue;
		if ((int)i == selected)
			ngui_fill_rect(g, (struct ngui_rect){r.x, y, row_w,
						       FM_ROW_H - 1},
				       g->theme.selection);
		else if ((int)i == hover_idx)
			ngui_fill_rect(g, (struct ngui_rect){r.x, y, row_w,
						       FM_ROW_H - 1},
				       g->theme.panel_alt);
		ngui_icon(g, (struct ngui_rect){r.x + 4, y + 3, 16, 16},
			  e->is_mount ? NGUI_ICON_DISK :
			  e->is_dir ? NGUI_ICON_FOLDER : NGUI_ICON_FILE,
			  e->is_mount ? g->theme.icon_disk :
			  e->is_dir ? g->theme.icon_folder : g->theme.icon_file);

		if (e->is_mount) {
			s_append(detail, &dn, e->detail, sizeof(detail));
		} else if (e->is_dir) {
			s_append(detail, &dn, "<DIR>", sizeof(detail));
		} else {
			format_size(e->size, detail, sizeof(detail));
		}
		detail_x = r.x + row_w - 6 - (int)ngui_text_width(detail);
		if (detail_x < r.x + 30) {
			detail[0] = '\0';
			detail_x = r.x + row_w;
		}
		ngui_text_ellipsis(g, (struct ngui_rect){r.x + 28, y,
					 detail_x - r.x - 30, FM_ROW_H},
				   r.x + 28, y + 3, e->name,
				   g->theme.text);
		if (detail[0])
			ngui_text(g, detail_x, y + 3, detail,
				  g->theme.text_dim);
	}
	if (!entry_count)
		ngui_text(g, r.x + 10, r.y + 8, "Empty directory",
			  g->theme.text_dim);
	ngui_scroll_end(g, r, &list_scroll);

	if (press_idx >= 0) {
		unsigned int now = u_ticks_ms();

		select_index((unsigned int)press_idx);
		if (press_idx == last_click_index &&
		    now - last_click_ms < 500) {
			*action |= ACT_OPEN;
			last_click_index = -1;
		} else {
			last_click_index = press_idx;
			last_click_ms = now;
		}
	}
}

static void draw_status(struct ngui *g, struct ngui_rect r)
{
	char buf[160];
	unsigned int n = 0;
	unsigned short color = g->theme.text;

	ngui_fill_rect(g, r, g->theme.panel);
	ngui_fill_rect(g, (struct ngui_rect){r.x, r.y, r.w, 1},
		       g->theme.border);

	if (status_text[0]) {
		s_append(buf, &n, status_text, sizeof(buf));
		if (status_error)
			color = g->theme.error;
	} else {
		s_append_uint(buf, &n, entry_count, sizeof(buf));
		s_append(buf, &n, entry_count == 1 ? " item" : " items", sizeof(buf));
		if (truncated)
			s_append(buf, &n, " (first 256)", sizeof(buf));
		if (entry_count && selected >= 0 &&
		    selected < (int)entry_count) {
			s_append(buf, &n, "   ", sizeof(buf));
			s_append(buf, &n, entries[selected].name, sizeof(buf));
			if (entries[selected].is_mount) {
				s_append(buf, &n, "  ", sizeof(buf));
				s_append(buf, &n, entries[selected].detail,
					 sizeof(buf));
			} else if (!entries[selected].is_dir) {
				char sz[20];

				format_size(entries[selected].size, sz, sizeof(sz));
				s_append(buf, &n, "  ", sizeof(buf));
				s_append(buf, &n, sz, sizeof(buf));
			}
		}
	}
	ngui_text_ellipsis(g, r, r.x + 6, r.y + (r.h - 16) / 2, buf, color);
}

static void draw_modal(struct ngui *g, unsigned int *action)
{
	struct ngui_rect all = {0, 0, g->w, g->h};
	int pw = g->w - 24;
	int ph = g->h - 16;
	int x, y, title_y, label_y, input_y, buttons_y;
	int bw, bx;
	struct ngui_rect input;
	int res;

	if (pw > 340)
		pw = 340;
	if (pw < 160)
		pw = 160;
	if (ph > 130)
		ph = 130;
	if (ph < 96)
		ph = 96;
	x = (g->w - pw) / 2;
	y = (g->h - ph) / 2;
	title_y = y + (ph >= 120 ? 9 : 6);
	label_y = y + (ph >= 120 ? 30 : 22);
	input_y = y + (ph >= 120 ? 48 : 38);
	buttons_y = y + ph - 34;
	bw = (pw - 36) / 2;
	if (bw > 80)
		bw = 80;
	if (bw < 50)
		bw = 50;
	bx = x + (pw - (bw * 2 + 8)) / 2;

	ngui_fill_rect(g, all, g->theme.bg);
	ngui_fill_rect(g, (struct ngui_rect){x + 3, y + 3, pw, ph},
		       NGUI_RGB(8, 10, 16));
	ngui_fill_rect(g, (struct ngui_rect){x, y, pw, ph}, g->theme.panel);
	ngui_frame(g, (struct ngui_rect){x, y, pw, ph}, g->theme.accent);
	ngui_text(g, x + 12, title_y, "New folder", g->theme.text);
	ngui_text(g, x + 12, label_y, "Name", g->theme.text_dim);

	input = (struct ngui_rect){x + 12, input_y, pw - 24, 22};
	res = ngui_text_input(g, ID_NEW_INPUT, input, &new_in);
	if (res & NGUI_INPUT_SUBMIT)
		*action |= ACT_CREATE;
	if (res & NGUI_INPUT_CANCEL)
		*action |= ACT_CANCEL;
	if (buttons_y > input_y + 44)
		ngui_text(g, x + 12, input_y + 28,
			  "Enter creates, Esc cancels", g->theme.text_dim);

	if (ngui_button(g, ID_CREATE,
			(struct ngui_rect){bx, buttons_y, bw, 24}, "Create"))
		*action |= ACT_CREATE;
	if (ngui_button(g, ID_CANCEL,
			(struct ngui_rect){bx + bw + 8, buttons_y, bw, 24},
			"Cancel"))
		*action |= ACT_CANCEL;
}

static void draw(struct ngui *g, unsigned int *action)
{
	struct ngui_rect r = {0, 0, g->w, g->h};
	struct ngui_rect toolbar, path, status, body;
	int side_w;

	if (modal) {
		draw_modal(g, action);
		return;
	}
	toolbar = ngui_cut_top(&r, FM_TOOLBAR_H);
	path = ngui_cut_top(&r, FM_PATH_H);
	status = ngui_cut_bottom(&r, FM_STATUS_H);
	body = r;

	draw_toolbar(g, toolbar, action);
	draw_path(g, path, action);
	if (body.w > 0 && body.h > 0) {
		side_w = g->w >= 520 ? FM_SIDEBAR_W : 0;
		if (side_w) {
			struct ngui_rect side = ngui_cut_left(&body, side_w);

			draw_sidebar(g, side, action);
		}
		draw_list(g, body, action);
	}
	draw_status(g, status);
}

static void handle_keys(struct ngui *g, unsigned int *action)
{
	int k;

	if (ngui_focus(g))
		return;
	if (ngui_scroll_keys(g, &list_scroll))
		ngui_invalidate(g);
	while ((k = ngui_key(g)) != 0) {
		switch (k) {
		case NGUI_KEY_UP:
			if (entry_count && selected > 0)
				select_index((unsigned int)(selected - 1));
			break;
		case NGUI_KEY_DOWN:
			if (entry_count &&
			    selected + 1 < (int)entry_count)
				select_index((unsigned int)(selected + 1));
			break;
		case NGUI_KEY_HOME:
			if (entry_count)
				select_index(0);
			break;
		case NGUI_KEY_END:
			if (entry_count)
				select_index(entry_count - 1);
			break;
		case '\n':
		case '\r':
			*action |= ACT_OPEN;
			break;
		case NGUI_KEY_BACKSPACE:
			*action |= ACT_PARENT;
			break;
		case NGUI_KEY_TAB:
		case 0x0C:              /* Ctrl+L */
			ngui_set_focus(g, ID_PATH);
			break;
		case 0x0E:              /* Ctrl+N */
			*action |= ACT_NEW;
			break;
		case 0x12:              /* Ctrl+R */
			*action |= ACT_REFRESH;
			break;
		case NGUI_KEY_ESC:
			*action |= ACT_CLOSE;
			break;
		default:
			break;
		}
	}
}

void _start(const char *arg)
{
	struct win_info wi;
	char start[FM_PATH_MAX];
	int quit = 0;

	(void)arg;
	if (u_win_create(600, 400, "Files", &wi) != 0) {
		u_puts("files: no graphics window\n");
		u_exit(1);
	}
	ngui_init(&ui, (unsigned short *)wi.fb, (int)wi.width, (int)wi.height);
	path_in.placeholder = "/path";
	new_in.placeholder = "NAME";
	list_scroll.offset = 0;
	load_mounts();

	if (u_getcwd(start, sizeof(start)) != 0 || !start[0])
		s_copy(start, "/", sizeof(start));
	if (load_path(start) != 0)
		load_path("/");

	for (;;) {
		unsigned int action = 0;
		unsigned int now;

		now = u_ticks_ms();
		if (status_until && (int)(now - status_until) >= 0) {
			status_text[0] = '\0';
			status_until = 0;
			status_error = 0;
			ngui_invalidate(&ui);
		}
		if (!ngui_begin(&ui)) {
			u_sleep_ms(10);
			continue;
		}
		draw(&ui, &action);
		if (modal) {
			int k = ngui_key_peek(&ui);

			if (k == NGUI_KEY_ESC) {
				ngui_key(&ui);
				action |= ACT_CANCEL;
			}
		} else {
			handle_keys(&ui, &action);
		}
		ngui_end(&ui);
		if (action)
			do_action(action);
		if (action & ACT_CLOSE)
			quit = 1;
		if (quit)
			break;
		u_sleep_ms(10);
	}
	u_win_close();
	u_exit(0);
}
