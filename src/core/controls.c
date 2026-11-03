/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The controls in the titlebar: three checkboxes and a size drop-down; a
 * GPU drop-down before the size one, dim where the renderer lists one GPU;
 * an output drop-down before that, naming the monitor the window is on and
 * dim where there is one, or where the bar cannot move the window
 * (Wayland's) -- under a window server it gives way where the window is
 * too narrow for it (ctl_output_in()); and where the window is the output
 * (KMS) the size list one of display modes, and no *Fs Upsample* box
 * (ctl_check_shown()).
 *
 * The library's own, because each drives a knob the library already owns --
 * FSAA, MLAA, the render-scale pin, the window size, the GPU
 * (fnl_request_gpu()), the output (fnl_request_output()), and where the
 * window is the output its display mode. They call the same public
 * knobs a caller would, so the event that reports a change comes out of the
 * frame's diff (src/core/events.c) either way.
 *
 * The size list is derived, not shipped: standard sizes filtered against the
 * size floor and the room the output has. Where the window is the output it
 * is the output's own modes instead, refresh and all (fnl_mode_get()), and
 * a pick sets the mode (fnl_request_mode()). The output list is the
 * outputs the window can be on (fnl_request_output()): the card's on KMS,
 * the window server's monitors elsewhere.
 *
 * Every rectangle is a pure function of the window width, computed on both
 * threads, for deco_reserved_rect()'s reason. DESIGN *The titlebar controls*.
 */

#include "core/app.h"

/* ---- geometry -------------------------------------------------------------
 *
 * No height among these: the run's height is the bar's, so the boxes line up
 * with the buttons beside them. */
#define CTL_COMBO_PAD        5
#define CTL_COMBO_ARROW_W    6
#define CTL_ITEM_H          18
#define CTL_ITEM_PAD         8
#define CTL_CHECK_BOX       12     /* the square itself      */
#define CTL_CHECK_GAP        6     /* square to label        */
#define CTL_CHECK_PAD        8     /* label to what is right */

/* ---- the sizes on offer -------------------------------------------------- */

/* CTL_MAX_ITEMS bounds a drop-down's list. A fixed array because it is
 * filled from the hit test as well as the draw -- the input path, where an
 * allocation is a stall. The output's modes are cut further, to the rows
 * that fit below the bar (ctl_rows_fit()). */
#define CTL_MAX_ITEMS 32
_Static_assert(CTL_MAX_ITEMS <= OUTPUT_MODES_MAX,
			   "the titlebar's list is read through a buffer of that size");

/* A size, and where it is a mode its refresh; 0 for a standard size. */
typedef struct { int w, h; unsigned int mhz; } CtlRes;

/* Which drop-down is open: none, the size box, the output box or the GPU
 * box. */
enum { CTL_SHUT = 0, CTL_OPEN_SIZE = 1, CTL_OPEN_OUTPUT = 2,
	   CTL_OPEN_GPU = 3 };
/* What ctl_hit() answers past the checkboxes: the three boxes. */
enum { CTL_HIT_SIZE = CTL_CHECK_COUNT, CTL_HIT_OUTPUT = CTL_CHECK_COUNT + 1,
	   CTL_HIT_GPU = CTL_CHECK_COUNT + 2 };

static const struct { int w, h; } CTL_RES[] = {
	{  640,  480 }, {  800,  600 }, { 1024,  768 }, { 1280,  720 },
	{ 1280,  800 }, { 1366,  768 }, { 1280,  960 }, { 1440,  900 },
	{ 1600,  900 }, { 1400, 1050 }, { 1680, 1050 }, { 1600, 1200 },
	{ 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 },
	{ 3840, 2160 },
};
#define CTL_RES_COUNT ((int)(sizeof CTL_RES / sizeof CTL_RES[0]))

/* The nearest of three named ratios, not a reduction of w:h: 1366x768
 * reduces to 683:384, which everyone calls 16:9. */
static const char *ctl_aspect(int w, int h)
{
	static const struct { int num, den; const char *name; } A[] = {
		{ 4, 3, "4:3" }, { 16, 10, "16:10" }, { 16, 9, "16:9" },
	};
	const char *best = A[0].name;
	double err = 1e9;
	for (int i = 0; i < (int)(sizeof A / sizeof A[0]); i++) {
		const double want = (double)A[i].num / A[i].den;
		const double got  = (double)w / (h ? h : 1);
		const double e = got > want ? got - want : want - got;
		if (e < err) { err = e; best = A[i].name; }
	}
	return best;
}

/* A size as the list names it. The widest is CTL_SIZE_WIDEST, which is what
 * the box is sized for, or CTL_MODE_WIDEST where the rows are modes;
 * CTL_SIZE_MAX holds any. An output's name is cut to CTL_OUTPUT_WIDEST's
 * width, which the kernel's names ("HDMI-A-1", "eDP-1") are inside. */
#define CTL_SIZE_WIDEST   "3840x2160 (16:10)"
#define CTL_MODE_WIDEST   "7680x4320 @ 119.88 Hz"
#define CTL_OUTPUT_WIDEST "Virtual-10"
#define CTL_SIZE_MAX      40

/* The GPU box names the GPU in use by its number, as the startup log and
 * FNL_GPU count them -- a box wide enough for a name would put 112 points
 * more on the floor, past the 640 offered as the smallest area -- and its
 * list names each in full, with its number and its kind, as wide as the
 * widest row, up to CTL_GPU_ROW_MAX bytes. */
#define CTL_GPU_WIDEST    "GPU 8"
#define CTL_GPU_ROW_MAX   112
/* The longest a row's GPU name is drawn: "NVIDIA GeForce GTX 1050 Ti on
 * nouveau" is 37; with the number, the words and the figure, the row is
 * then within CTL_GPU_ROW_MAX whatever an int prints. */
#define CTL_GPU_NAME_MAX  56
_Static_assert(GPU_LIST_MAX <= 9, "CTL_GPU_WIDEST holds one digit");

/* A refresh as a person reads it: 60, 59.94, 74.97 -- hundredths, and no
 * trailing zero. */
static void ctl_hz(char *buf, size_t n, unsigned int mhz)
{
	const unsigned int c = (mhz + 5) / 10;
	if (c % 100 == 0)
		snprintf(buf, n, "%u", c / 100);
	else if (c % 10 == 0)
		snprintf(buf, n, "%u.%u", c / 100, c % 100 / 10);
	else
		snprintf(buf, n, "%u.%02u", c / 100, c % 100);
}

static void ctl_size_name(char *buf, size_t n, const CtlRes *r)
{
	if (r->mhz) {
		char hz[16];
		ctl_hz(hz, sizeof hz, r->mhz);
		snprintf(buf, n, "%dx%d @ %s Hz", r->w, r->h, hz);
	} else {
		snprintf(buf, n, "%dx%d (%s)", r->w, r->h, ctl_aspect(r->w, r->h));
	}
}

/* ---- what the two threads share -------------------------------------------
 *
 * The state is atomic because both threads touch it. The measurements come
 * from the *published* block because both have to filter the size list to
 * the same length, or a row is drawn where it cannot be clicked. */
#define CTL_CHECKED(a, i)  fnl_atomic_load_bool(&(a)->ctl_check[i])
#define CTL_OPEN(a)        ((int)fnl_atomic_load_u32(&(a)->ctl_combo_open))
#define CTL_PIN_W(a)       ((int)fnl_atomic_load_u32(&(a)->ctl_pin_w))
#define CTL_PIN_H(a)       ((int)fnl_atomic_load_u32(&(a)->ctl_pin_h))
#define CTL_HOVER(a) \
	((int)fnl_atomic_load_u32(&(a)->ctl_combo_hover1) - 1)

static void ctl_set_hover(App *a, int row)
{
	fnl_atomic_store_u32(&a->ctl_combo_hover1, (uint32_t)(row + 1));
}

/* A list opening or shutting. Either way no row is under the pointer until
 * it next moves, and the flag is state the frame comparison cannot see. */
static void ctl_set_open(App *a, int which)
{
	fnl_atomic_store_u32(&a->ctl_combo_open, (uint32_t)which);
	ctl_set_hover(a, -1);
	fnl_invalidate(a);
}

/* The window's measurements, taken together under App::lock: read apart, the
 * scale and the output could come from two frames. The floor is read inside
 * it too, because deco_min_window() reads fields deco_apply_custom() writes
 * under this lock from the pump's thread. */
typedef struct { int s120, out_w, out_h, bar, min_w, min_h; } CtlEnv;

static CtlEnv ctl_env(const App *a)
{
	App *m = (App *)a;                 /* the lock is not const */
	CtlEnv e;
	fnl_mutex_lock(&m->lock);
	e.s120  = a->pub_scale120 > 0 ? a->pub_scale120 : 120;
	e.out_w = a->pub_out_w_pt;
	e.out_h = a->pub_out_h_pt;
	e.bar   = a->pub_bar_h;
	deco_min_window(a, false, &e.min_w, &e.min_h);
	fnl_mutex_unlock(&m->lock);
	/* Before the first frame is published there is nothing to filter against
	 * yet; the bar's own height is the honest stand-in and the output bound
	 * simply does not apply. */
	if (e.bar <= 0)
		e.bar = DECO_TITLEBAR_H;
	return e;
}

/* How many rows of a list fit below the bar in a window of the output's
 * height: an output's modes run to thirty, and a 720x400 mode has room for
 * nineteen. Both threads work from the published height, so both cut the
 * list alike. */
static int ctl_rows_fit(const CtlEnv *e)
{
	if (e->out_h <= 0)
		return CTL_MAX_ITEMS;
	const int room = (e->out_h - e->bar) / CTL_ITEM_H;
	return room < 1 ? 1 : room < CTL_MAX_ITEMS ? room : CTL_MAX_ITEMS;
}

/* A run of `fit` rows out of `n`, from the top, slid down to hold row `cur`
 * where that is below it; the first row shown. */
static int ctl_rows_first(int n, int fit, int cur)
{
	if (n <= fit || cur < fit)
		return 0;
	return cur - fit + 1 < n - fit ? cur - fit + 1 : n - fit;
}

/* The output's modes, where the window is the output: whole windows and not
 * areas, since a mode is the whole window, and in the backend's order --
 * largest first, fastest first within a size -- cut to the rows that fit.
 * `*now` is the row in force, or -1. */
static int ctl_modes(const App *a, const CtlEnv *e, CtlRes *out, int max,
					 int *now)
{
	FnlMode modes[OUTPUT_MODES_MAX];
	int cur;
	const int got = BACKEND(a)->modes(a, modes, OUTPUT_MODES_MAX, &cur);
	const int fit = ctl_rows_fit(e) < max ? ctl_rows_fit(e) : max;
	const int first = ctl_rows_first(got, fit, cur);
	int n = 0;
	*now = -1;
	for (int i = first; i < got && n < fit; i++) {
		if (i == cur)
			*now = n;
		out[n].w = modes[i].w_px;
		out[n].h = modes[i].h_px;
		out[n].mhz = modes[i].refresh_mhz;
		n++;
	}
	return n;
}

/* The outputs the window can be on, cut to the rows that fit; `*now` the row
 * it is on, or -1. */
static int ctl_outputs(const App *a, const CtlEnv *e,
					   char (*out)[OUTPUT_NAME_MAX], int *now)
{
	char names[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	int cur;
	const int got = BACKEND(a)->outputs(a, names, OUTPUTS_MAX, &cur);
	const int fit = ctl_rows_fit(e) < OUTPUTS_MAX ? ctl_rows_fit(e)
												  : OUTPUTS_MAX;
	const int first = ctl_rows_first(got, fit, cur);
	int n = 0;
	*now = -1;
	for (int i = first; i < got && n < fit; i++) {
		if (i == cur)
			*now = n;
		memcpy(out[n], names[i], OUTPUT_NAME_MAX);
		n++;
	}
	return n;
}

/* The GPUs the list holds (fnl_gpu_count()), each as a row names it -- its
 * short name and its kind, its renderer where the list holds GPUs of more
 * than one, and its measured frame rate where it is ranked -- cut to the
 * rows that fit; `*now` the row in use, or -1.
 * A GPU that can move to another kernel driver is named alike in each of
 * its rows, by the name it keeps on either driver, with the driver the row
 * draws on: "<GPU> on nouveau" -- each renderer on each driver one row,
 * where its renderers' own names for it differ ("/PCIe/SSE2", Zink's).
 * Read under App::lock in one go, so the rows and `now` are of one list.
 * Rows count from the list's first, so a row is its index. */

static int ctl_gpus(const App *a, const CtlEnv *e,
					char (*out)[CTL_GPU_ROW_MAX], int *now)
{
	App *m = (App *)a;                 /* the lock is not const */
	GpuEntry list[GPU_LIST_MAX];
	char moving[GPU_NAME_MAX];
	fnl_mutex_lock(&m->lock);
	const int got = a->gpu_list_n;
	*now = a->gpu_list_cur;
	memcpy(list, a->gpu_list, sizeof list);
	memcpy(moving, a->gpu_driver_gpu, sizeof moving);
	fnl_mutex_unlock(&m->lock);

	const int fit = ctl_rows_fit(e) < GPU_LIST_MAX ? ctl_rows_fit(e)
												   : GPU_LIST_MAX;
	const int n = got < fit ? got : fit;
	if (*now >= n)
		*now = -1;
	bool mixed = false;
	for (int i = 1; i < n; i++)
		mixed |= list[i].renderer != list[0].renderer ||
				 strcmp(list[i].via, list[0].via) != 0;
	for (int i = 0; i < n; i++) {
		char cut[GPU_NAME_MAX];
		char named[GPU_NAME_MAX + GPU_DRIVER_NAME_MAX + 4];
		const char *name = app_gpu_short(cut, sizeof cut, list[i].name);
		if (list[i].driver[0] && moving[0]) {
			snprintf(named, sizeof named, "%s on %s",
					 app_gpu_short(cut, sizeof cut, moving),
					 list[i].driver);
			name = named;
		}
		/* A ranked entry says its figure, which is what orders the list
		 * (src/core/rank.c). */
		char fps[24] = "";
		if (list[i].fps > 0.0f)
			snprintf(fps, sizeof fps, "  %.0f fps", (double)list[i].fps);
		/* Each part held to a length the row has room for together: the
		 * name to CTL_GPU_NAME_MAX, the kind and renderer words to their
		 * longest ("integrated", "Vulkan via D3D11"), the figure to its
		 * own. */
		char how[32];
		if (list[i].via[0])
			snprintf(how, sizeof how, "%s via %s",
					 app_renderer_name(list[i].renderer), list[i].via);
		else
			snprintf(how, sizeof how, "%s",
					 app_renderer_name(list[i].renderer));
		if (mixed)
			snprintf(out[i], CTL_GPU_ROW_MAX, "%d  %.*s (%.10s, %.16s)%.12s",
					 i + 1, CTL_GPU_NAME_MAX, name,
					 app_gpu_kind_word(list[i].kind), how, fps);
		else
			snprintf(out[i], CTL_GPU_ROW_MAX, "%d  %.*s (%.10s)%.12s", i + 1,
					 CTL_GPU_NAME_MAX, name,
					 app_gpu_kind_word(list[i].kind), fps);
	}
	return n;
}

/* How many GPUs the renderer lists. The box is in the run once there is a
 * list at all, one GPU included -- drawn dim and inert then, naming the GPU
 * in use -- so the run and the floor (deco_min_window()) do not change with
 * the machine. The atomic and not the list's count under App::lock, which
 * the floor is read with already held (ctl_env()). */
static int ctl_gpu_count(const App *a)
{
	return (int)fnl_atomic_load_u32(&a->gpu_count);
}

/* What the box reads: the GPU in use by its number, whose name the list
 * beneath gives -- or no number where none of the list is in use (WARP
 * taken where Direct3D 12 listed nothing). */
static void ctl_gpu_label(const App *a, char *buf, size_t n)
{
	App *m = (App *)a;                 /* the lock is not const */
	fnl_mutex_lock(&m->lock);
	const int cur = a->gpu_list_cur;
	fnl_mutex_unlock(&m->lock);
	if (cur >= 0)
		snprintf(buf, n, "GPU %d", cur + 1);
	else
		snprintf(buf, n, "GPU");
}

/* Bounded by the room the output has and by the published floor. The floor
 * is stated on the *area* and the output on the *window*, so the bar's height
 * is added for one comparison and not the other. */
static int ctl_options(const App *a, CtlRes *out, int max, int *now)
{
	const CtlEnv e = ctl_env(a);
	*now = -1;
	if (BACKEND(a)->is_output)
		return ctl_modes(a, &e, out, max, now);
	/* The floor is window geometry; the area's floor is that less the bar. */
	const int area_min_h = e.min_h > e.bar ? e.min_h - e.bar : 1;

	int n = 0;
	for (int i = 0; i < CTL_RES_COUNT && n < max; i++) {
		const int aw = res_window_w_pt(CTL_RES[i].w, e.s120);
		const int ah = res_window_h_pt(CTL_RES[i].h, e.s120, 0);
		if (aw < e.min_w || ah < area_min_h)
			continue;
		if (e.out_w > 0 && (aw > e.out_w || ah + e.bar > e.out_h))
			continue;
		out[n].w = CTL_RES[i].w;
		out[n].h = CTL_RES[i].h;
		out[n].mhz = 0;
		n++;
	}
	return n;
}

/* ---- layout ---------------------------------------------------------------
 *
 * TEXT_BODY is monospaced and every string measured here is ASCII, so a width
 * is a constant count times the advance. ctl_run_w() is asked on every
 * pointer motion over the bar, through deco_zone_at(). */

#define CTL_LABEL_OF(s) { (s), (int)(sizeof (s) - 1) }
static const struct { const char *text; int chars; }
CTL_LABEL[CTL_CHECK_COUNT] = {
	CTL_LABEL_OF("FSAA"), CTL_LABEL_OF("MLAA"), CTL_LABEL_OF("Fs Upsample"),
};

/* Whether a checkbox is in the run. Where the window is the output the size
 * list is of display modes and never sets the scene's pin, so *Fs Upsample*
 * would have nothing to hold the scene at but the size already in force:
 * it is left out, and fnl_set_scene_size_px() is how a program pins one
 * there. */
static bool ctl_check_shown(const App *a, int i)
{
	return i != CTL_CHECK_SCALE || !BACKEND(a)->is_output;
}

/* A checkbox's width in the run, 0 for one left out. */
static int ctl_check_w(const App *a, int i)
{
	if (!ctl_check_shown(a, i))
		return 0;
	return CTL_CHECK_BOX + CTL_CHECK_GAP +
		   CTL_LABEL[i].chars * text_advance(TEXT_BODY) + CTL_CHECK_PAD;
}

static int ctl_checks_w(const App *a)
{
	int w = 0;
	for (int i = 0; i < CTL_CHECK_COUNT; i++)
		w += ctl_check_w(a, i);
	return w;
}

/* A box wide enough for a label of this many characters. */
static int ctl_box_w(int chars)
{
	return CTL_COMBO_PAD + chars * text_advance(TEXT_BODY) +
		   CTL_COMBO_PAD + CTL_COMBO_ARROW_W + CTL_COMBO_PAD;
}

/* The size box: wide enough for the widest label it can hold. */
static int ctl_combo_w(const App *a)
{
	return ctl_box_w(BACKEND(a)->is_output
						 ? (int)(sizeof CTL_MODE_WIDEST - 1)
						 : (int)(sizeof CTL_SIZE_WIDEST - 1));
}

/* The output box with the gap after it. */
static int ctl_output_room(void)
{
	return ctl_box_w((int)(sizeof CTL_OUTPUT_WIDEST - 1)) + CTL_CHECK_PAD;
}

/* The GPU box, and the gap after it; neither before a renderer has listed
 * its GPUs. */
static int ctl_gpu_w(const App *a)
{
	return ctl_gpu_count(a) > 0
			   ? ctl_box_w((int)(sizeof CTL_GPU_WIDEST - 1))
			   : 0;
}

static int ctl_gpu_gap(const App *a)
{
	return ctl_gpu_count(a) > 0 ? CTL_CHECK_PAD : 0;
}

/* The run as the floor holds it (deco_min_window()): the output box in it
 * only where the window is the output. */
int ctl_run_w(const App *a)
{
	return ctl_checks_w(a) +
		   (BACKEND(a)->is_output ? ctl_output_room() : 0) +
		   ctl_gpu_w(a) + ctl_gpu_gap(a) +
		   ctl_combo_w(a);
}

/* Whether the output box is in the run of a window w wide. Where the window
 * is the output, always, and the floor holds it. Under a window server it
 * is the one control that gives way: the floor there is 625 against
 * CONTENT_MIN_W's 640, and the box's 109 on it would empty the size list
 * of 640x480 (DESIGN *The size floor*). So it joins the run where the
 * window has room for it beside the rest, and the floor is the run
 * without it. */
static bool ctl_output_in(const App *a, int w)
{
	return BACKEND(a)->is_output ||
		   w >= DECO_MIN_W + DECO_BTN_GROUP_GAP + ctl_run_w(a) +
					ctl_output_room();
}

/* The output box, and the gap after it; neither where it gave way. */
static int ctl_output_w(const App *a, int w)
{
	return ctl_output_in(a, w)
			   ? ctl_box_w((int)(sizeof CTL_OUTPUT_WIDEST - 1))
			   : 0;
}

static int ctl_output_gap(const App *a, int w)
{
	return ctl_output_in(a, w) ? CTL_CHECK_PAD : 0;
}

int ctl_run_w_at(const App *a, int w)
{
	return ctl_run_w(a) + (!BACKEND(a)->is_output && ctl_output_in(a, w)
							   ? ctl_output_room()
							   : 0);
}

/* Where each box starts, from the run's left edge. */
static int ctl_output_x(const App *a, int rx)
{
	return rx + ctl_checks_w(a);
}

static int ctl_gpu_x(const App *a, int w, int rx)
{
	return ctl_output_x(a, rx) + ctl_output_w(a, w) + ctl_output_gap(a, w);
}

static int ctl_size_x(const App *a, int w, int rx)
{
	return ctl_gpu_x(a, w, rx) + ctl_gpu_w(a) + ctl_gpu_gap(a);
}

int ctl_run_x(const App *a, int w)
{
	int  min_x, max_x, full_x, close_x, by;
	bool btn_vis;
	deco_button_rects(a, w, &min_x, &max_x, &full_x, &close_x, &by,
					  &btn_vis);

	const int x = full_x - DECO_BTN_GROUP_GAP - ctl_run_w_at(a, w);
	return btn_vis && x > DECO_BTN_MARGIN ? x : -1;
}

/* Where the run sits: right-to-left from just left of the window buttons.
 * Its place in the give-way chain is stated at deco_reserved_rect(). */
static bool ctl_run_rect(const App *a, int w, int *x, int *y, int *rw,
						 int *rh)
{
	int  min_x, max_x, full_x, close_x, by;
	bool btn_vis;
	deco_button_rects(a, w, &min_x, &max_x, &full_x, &close_x, &by,
					  &btn_vis);

	*rw = ctl_run_w_at(a, w);
	*rh = DECO_ITEM_H;
	*y  = by;
	*x  = ctl_run_x(a, w);
	return *x >= 0;
}

/* How many sizes survive the filter. With none the drop-down is drawn dim
 * and is inert. */
static int ctl_size_count(const App *a)
{
	CtlRes opts[CTL_MAX_ITEMS];
	int now;
	return ctl_options(a, opts, CTL_MAX_ITEMS, &now);
}

/* How many outputs the box offers. With one there is nowhere to move to,
 * and where the backend moves only a fullscreen window -- Wayland's, whose
 * bar is never shown then -- nothing the bar can move; either way the box
 * is drawn dim and is inert, naming the output the window is on. */
static int ctl_output_count(const App *a)
{
	if (BACKEND(a)->outputs_fullscreen_only)
		return 0;
	const CtlEnv e = ctl_env(a);
	char names[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	int now;
	return ctl_outputs(a, &e, names, &now);
}

/* Whether a checkbox can act. All three need the offscreen target; FSAA also
 * needs a sample count worth having, and the scaling box a size to hold.
 * `n_sizes` is passed in because the frame path has already built the list,
 * and filtering it again is another crossing of App::lock. */
static bool ctl_check_enabled(const App *a, int i, int n_sizes)
{
	const FnlRendererVtbl *r = renderer_any(a);
	if (!r || !r->offscreen_scene)
		return false;
	if (i == CTL_CHECK_FSAA)
		return r->max_samples(a) > 1;
	if (i == CTL_CHECK_SCALE)
		return n_sizes > 0;
	return true;
}

/* ---- what the box reads out ----------------------------------------------
 *
 * The size the scene is held at while the tick is on, and the presentable
 * area's while it is off. The size in force rather than the one picked: a pin
 * larger than the area is refused, and the box must not claim it. */
static void ctl_combo_label(const App *a, const FnlDrawState *ds,
							int area_w, int area_h, char *buf, size_t n)
{
	/* Where the list is of modes, the box names the mode: the scene size is
	 * the checkbox's to show, and the mode is what the list picks. */
	if (BACKEND(a)->is_output) {
		FnlMode m;
		if (fnl_mode_current(a, &m)) {
			const CtlRes r = { m.w_px, m.h_px, m.refresh_mhz };
			ctl_size_name(buf, n, &r);
			return;
		}
	}
	int w = ds->scene_w, h = ds->scene_h;
	if (!CTL_CHECKED(a, CTL_CHECK_SCALE) || w <= 0) {
		w = area_w;
		h = area_h;
	}
	/* The ratio only for a size the list itself offers: ctl_aspect() names the
	 * nearest of three for any size at all, and calling a dragged 1000x700
	 * "4:3" would be the box stating something the window is not. */
	for (int i = 0; i < CTL_RES_COUNT; i++)
		if (CTL_RES[i].w == w && CTL_RES[i].h == h) {
			const CtlRes r = { w, h, 0 };
			ctl_size_name(buf, n, &r);
			return;
		}
	snprintf(buf, n, "%dx%d", w, h);
}

/* The output box names the output the window is on as its list names it,
 * so the row marked below the box and the box agree; and follows it, a
 * move by the bar, by fnl_request_output() or by the window server alike,
 * since the backend publishes the list whenever the window changes output.
 * fnl_output_name() where the list has no row for it yet: on Wayland that
 * name is every output the window covers, which the box would cut. */
static void ctl_output_label(const App *a, char *buf, size_t n)
{
	char names[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	int cur;
	const int got = BACKEND(a)->outputs(a, names, OUTPUTS_MAX, &cur);
	snprintf(buf, n, "%s",
			 cur >= 0 && cur < got ? names[cur] : fnl_output_name(a));
}

/* ---- drawing ------------------------------------------------------------- */

/* Whether box `i` is ticked. The two antialiasing boxes show the mode the
 * frame was latched with, whoever set it -- a tick of their own would stay
 * off when the caller turns FSAA on with fnl_set_antialias(). The scaling
 * box keeps its own: the size it holds is the drop-down's pick, which the
 * library's state does not record. */
static bool ctl_ticked(const App *a, const FnlDrawState *ds, int i)
{
	if (i == CTL_CHECK_FSAA)
		return ds->aa == FNL_AA_MSAA;
	if (i == CTL_CHECK_MLAA)
		return ds->aa == FNL_AA_MLAA;
	return CTL_CHECKED(a, i);
}

static void ctl_draw_check(App *a, int i, int x, int y, int h, bool ticked,
						   bool enabled, bool focused)
{
	const bool on   = ticked && enabled;
	const bool live = enabled && focused;
	const float *edge = a->pal[live ? COL_LINE_ON : COL_LINE_OFF];
	const float *ink  = a->pal[live ? COL_ICON_ON : COL_CTL_DIM];
	/* A tick on an unfocused bar is still a tick, so it keeps its mark and
	 * takes the same unfocused ink the button icons do. */
	const float *lit  = a->pal[focused ? COL_CTL_LIVE : COL_ICON_OFF];

	const int bx = x;
	const int by = y + (h - CTL_CHECK_BOX) / 2;
	overlay_box(a, (float)bx + 0.5f, (float)by + 0.5f,
				(float)(bx + CTL_CHECK_BOX) - 0.5f,
				(float)(by + CTL_CHECK_BOX) - 0.5f, 1.2f, on ? lit : edge);

	if (on)
		overlay_elbow(a, (float)bx + 2.5f, (float)by + 6.0f,
					  (float)bx + 5.0f, (float)by + 8.8f,
					  (float)bx + 9.5f, (float)by + 3.0f, 1.6f, lit);

	text_draw(a, bx + CTL_CHECK_BOX + CTL_CHECK_GAP,
			  y + (h - text_height(TEXT_BODY)) / 2,
			  TEXT_BODY, on ? lit : ink, CTL_LABEL[i].text);
}

/* A box. What it drops down is ctl_draw_popup() below, and the two are
 * drawn at opposite ends of the frame -- see the note there. The label is
 * cut to the box: an output's name is the kernel's, and could be long. */
static void ctl_draw_combo(App *a, int x, int y, int w, int h,
						   const char *label, bool active)
{
	const float *ink  = a->pal[active ? COL_ICON_ON : COL_CTL_DIM];
	const float *line = a->pal[COL_LINE_ON];

	overlay_rect(a, x, y, w, h, a->pal[COL_CTL_BOX]);
	overlay_frame(a, x, y, w, h, line);

	char cut[CTL_SIZE_MAX];
	const int fits = (w - 3 * CTL_COMBO_PAD - CTL_COMBO_ARROW_W) /
					 text_advance(TEXT_BODY);
	snprintf(cut, sizeof cut, "%.*s", fits > 0 ? fits : 0, label);
	text_draw(a, x + CTL_COMBO_PAD, y + (h - text_height(TEXT_BODY)) / 2,
			  TEXT_BODY, ink, cut);

	/* The little triangle, pointing the way the list will go. One bend rather
	 * than two strokes meeting at the apex, which would blend twice there and
	 * leave a bright dot on the tip (see draw_decorations). */
	const float ax = (float)(x + w - CTL_COMBO_PAD - CTL_COMBO_ARROW_W);
	const float ay = (float)(y + h / 2 - 1);
	overlay_elbow(a, ax, ay, ax + 3.0f, ay + 3.5f, ax + 6.0f, ay, 1.4f, ink);
}

/* From draw_decorations(), with the bar already painted. Draws nothing when
 * the run was refused, which only a window below its floor can see. */
void ctl_draw(App *a, const FnlDrawState *ds, int win_w,
			  int area_w, int area_h)
{
	int rx, ry, rw, rh;
	if (!ctl_run_rect(a, win_w, &rx, &ry, &rw, &rh))
		return;

	CtlRes opts[CTL_MAX_ITEMS];
	int now;
	const int n = ctl_options(a, opts, CTL_MAX_ITEMS, &now);

	int x = rx;
	for (int i = 0; i < CTL_CHECK_COUNT; i++) {
		if (!ctl_check_shown(a, i))
			continue;
		ctl_draw_check(a, i, x, ry, rh, ctl_ticked(a, ds, i),
					   ctl_check_enabled(a, i, n), ds->is_active);
		x += ctl_check_w(a, i);
	}
	if (ctl_output_w(a, win_w) > 0) {
		char on[OUTPUT_NAME_MAX];
		ctl_output_label(a, on, sizeof on);
		ctl_draw_combo(a, ctl_output_x(a, rx), ry, ctl_output_w(a, win_w),
					   rh, on, ds->is_active && ctl_output_count(a) > 1);
	}
	if (ctl_gpu_w(a) > 0) {
		char gpu[CTL_SIZE_MAX];
		ctl_gpu_label(a, gpu, sizeof gpu);
		ctl_draw_combo(a, ctl_gpu_x(a, win_w, rx), ry, ctl_gpu_w(a), rh, gpu,
					   ds->is_active && ctl_gpu_count(a) > 1);
	}
	char label[CTL_SIZE_MAX];
	ctl_combo_label(a, ds, area_w, area_h, label, sizeof label);
	ctl_draw_combo(a, ctl_size_x(a, win_w, rx), ry, ctl_combo_w(a), rh,
				   label, ds->is_active && n > 0);
}

/* The open list: which box it drops from, where, and its rows as text.
 * One function for the draw and both hit tests, so a row is drawn exactly
 * where it is clicked. False when no list is open or it has no rows. */
typedef struct {
	int    which;                   /* CTL_OPEN_SIZE, _OUTPUT or _GPU */
	int    x, y, w, n, now;         /* `now` the row in force, or -1 */
	CtlRes size[CTL_MAX_ITEMS];
	char   name[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	/* The GPU box's rows: gpu_n GPUs, and after them, unnumbered, the
	 * calibration the ranking offers (app_rank_recalibrates()). */
	char   gpu[GPU_LIST_MAX + 1][CTL_GPU_ROW_MAX];
	int    gpu_n;
} CtlList;

/* A list as wide as its widest row of `widest` characters where that is
 * wider than its box: it hangs from the box's right edge and grows to the
 * left, kept inside the window. A GPU's name is wider than its box, and so
 * is a monitor's where AppKit names it ("Built-in Retina Display"). */
static void ctl_list_hang(CtlList *l, int box_x, int box_w, int widest)
{
	l->w = 2 * CTL_ITEM_PAD + widest * text_advance(TEXT_BODY);
	if (l->w < box_w)
		l->w = box_w;
	l->x = box_x + box_w - l->w;
	if (l->x < 0)
		l->x = 0;
}

static bool ctl_list(const App *a, int win_w, CtlList *l)
{
	l->which = CTL_OPEN(a);
	if (l->which == CTL_SHUT)
		return false;
	int rx, ry, rw, rh;
	if (!ctl_run_rect(a, win_w, &rx, &ry, &rw, &rh))
		return false;
	l->y = ry + rh;
	if (l->which == CTL_OPEN_OUTPUT) {
		/* The box gave way while its list was open: a window narrowed
		 * under it. */
		if (ctl_output_w(a, win_w) <= 0)
			return false;
		const CtlEnv e = ctl_env(a);
		l->n = ctl_outputs(a, &e, l->name, &l->now);
		int widest = 0;
		for (int i = 0; i < l->n; i++) {
			const int c = (int)strlen(l->name[i]);
			widest = c > widest ? c : widest;
		}
		if (widest > CTL_GPU_ROW_MAX - 1)    /* what a row is drawn from */
			widest = CTL_GPU_ROW_MAX - 1;
		ctl_list_hang(l, ctl_output_x(a, rx), ctl_output_w(a, win_w),
					  widest);
	} else if (l->which == CTL_OPEN_GPU) {
		const CtlEnv e = ctl_env(a);
		l->n = l->gpu_n = ctl_gpus(a, &e, l->gpu, &l->now);
		/* The ranking measured again, where it is the library's to
		 * measure: a row of its own after the GPUs, with no number, so a
		 * GPU's row is its place in the list as before. */
		if (l->n > 0 && app_rank_recalibrates(a) && l->n < ctl_rows_fit(&e))
			snprintf(l->gpu[l->n++], CTL_GPU_ROW_MAX, "   Recalibrate: this "
					 "display now, the others when shown");
		int widest = 0;
		for (int i = 0; i < l->n; i++) {
			const int c = (int)strlen(l->gpu[i]);
			widest = c > widest ? c : widest;
		}
		ctl_list_hang(l, ctl_gpu_x(a, win_w, rx), ctl_gpu_w(a), widest);
	} else {
		l->x = ctl_size_x(a, win_w, rx);
		l->w = ctl_combo_w(a);
		l->n = ctl_options(a, l->size, CTL_MAX_ITEMS, &l->now);
	}
	return l->n > 0;
}

/* The open list, drawn from the end of draw_overlay() rather than with the
 * bar. "Over" is an append order here -- each kind of primitive is one draw
 * -- and the bar is painted before on_overlay runs, so a list drawn with it
 * would sit under the application's drawing while still owning every press
 * (see ctl_modal). The three flushes below issue everything appended so
 * far, which is what puts the list on top: the glyphs as well, or the
 * application's text, drawn after every background, shows through the
 * list's. */
FnlResult ctl_draw_popup(App *a, const FnlDrawState *ds, int win_w)
{
	/* The two conditions draw_decorations() returns on, restated because this
	 * is reached from the frame rather than from inside it: there is no list
	 * without a bar of ours for it to drop out of. One open as the bar went
	 * -- into fullscreen, or to the caller's own -- is put away here rather
	 * than coming back with the bar: nothing could be pressed on it while
	 * it was gone, so nothing races the store. No re-record: the frame
	 * already draws no list. */
	if (ds->custom_bar || !deco_client(ds->is_fullscreen)) {
		if (CTL_OPEN(a) != CTL_SHUT) {
			fnl_atomic_store_u32(&a->ctl_combo_open, (uint32_t)CTL_SHUT);
			ctl_set_hover(a, -1);
		}
		return FNL_OK;
	}
	if (!CTL_OPEN(a))
		return FNL_OK;

	static FNL_THREAD_LOCAL CtlList l;   /* 2.6 KiB: not on the stack */
	if (!ctl_list(a, win_w, &l))
		return FNL_OK;

	const int x = l.x, w = l.w, ly = l.y, n = l.n;
	const float *line = a->pal[COL_LINE_ON];

	overlay_flush_rects(a);
	overlay_flush_shapes(a);
	const FnlResult rc = text_end(a);

	overlay_rect(a, x, ly, w, n * CTL_ITEM_H, a->pal[COL_CTL_BOX]);
	for (int i = 0; i < n; i++) {
		const int iy = ly + i * CTL_ITEM_H;
		if (i == CTL_HOVER(a))
			overlay_rect(a, x + 1, iy, w - 2, CTL_ITEM_H,
						 a->pal[COL_CTL_HILITE]);

		char item[CTL_GPU_ROW_MAX];
		bool picked;
		if (l.which == CTL_OPEN_OUTPUT) {
			snprintf(item, sizeof item, "%.*s", (int)sizeof item - 1,
					 l.name[i]);
			picked = i == l.now;
		} else if (l.which == CTL_OPEN_GPU) {
			snprintf(item, sizeof item, "%s", l.gpu[i]);
			picked = i == l.now;
		} else {
			ctl_size_name(item, sizeof item, &l.size[i]);
			/* The size already picked, in the live ink: the box above shows
			 * the *area* while the tick is off, so nothing else marks it.
			 * Where the rows are modes, the mode in force. */
			picked = BACKEND(a)->is_output
						 ? i == l.now
						 : CTL_PIN_W(a) == l.size[i].w &&
							   CTL_PIN_H(a) == l.size[i].h;
		}
		text_draw(a, x + CTL_ITEM_PAD,
				  iy + (CTL_ITEM_H - text_height(TEXT_BODY)) / 2, TEXT_BODY,
				  a->pal[picked ? COL_CTL_LIVE : COL_ICON_ON], item);
	}
	overlay_rect(a, x, ly, 1, n * CTL_ITEM_H, line);
	overlay_rect(a, x + w - 1, ly, 1, n * CTL_ITEM_H, line);
	overlay_rect(a, x, ly + n * CTL_ITEM_H - 1, w, 1, line);
	return rc;
}

/* ---- the hit test --------------------------------------------------------
 *
 * Two entry points, because the open list is the one thing here that leaves
 * the titlebar's band: ctl_hit() for points in the band, ctl_popup_hit() for
 * a list that floats over the whole window. */

/* Which control is at a point in the band. -1 for none, 0..CTL_CHECK_COUNT-1
 * for a checkbox, CTL_HIT_SIZE for the size box, CTL_HIT_OUTPUT for the
 * output box, CTL_HIT_GPU for the GPU box. */
int ctl_hit(const App *a, int win_w, int x, int y)
{
	int rx, ry, rw, rh;
	if (!ctl_run_rect(a, win_w, &rx, &ry, &rw, &rh))
		return -1;
	if (y < ry || y >= ry + rh || x < rx || x >= rx + rw)
		return -1;

	int cx = rx;
	for (int i = 0; i < CTL_CHECK_COUNT; i++) {
		const int cw = ctl_check_w(a, i);
		if (cw && x >= cx && x < cx + cw - CTL_CHECK_PAD)  /* pad not live */
			return i;
		cx += cw;
	}
	const int ox = ctl_output_x(a, rx);
	if (x >= ox && x < ox + ctl_output_w(a, win_w))
		return CTL_HIT_OUTPUT;
	const int gx = ctl_gpu_x(a, win_w, rx);
	if (x >= gx && x < gx + ctl_gpu_w(a))
		return CTL_HIT_GPU;
	return x >= ctl_size_x(a, win_w, rx) ? CTL_HIT_SIZE : -1;
}

/* An open list holds the pointer only while the library's bar it drops
 * from is shown: under fnl_titlebar_custom() or in fullscreen it is not
 * drawn (ctl_draw_popup()), and a press must not land on rows nobody
 * sees. The pump's own view of the bar, as deco_hit() reads it. */
bool ctl_modal(const App *a)
{
	return CTL_OPEN(a) != CTL_SHUT && deco_client(a->is_fullscreen) &&
		   !a->deco_custom;
}

/* The row of the open list under a point, or -1. Answers for points anywhere
 * in the window: the list floats over it, and deco_press() asks here before
 * the band. */
int ctl_popup_hit(const App *a, int win_w, int x, int y)
{
	if (!ctl_modal(a))
		return -1;
	static FNL_THREAD_LOCAL CtlList l;
	if (!ctl_list(a, win_w, &l))
		return -1;
	if (x < l.x || x >= l.x + l.w || y < l.y || y >= l.y + l.n * CTL_ITEM_H)
		return -1;
	return (y - l.y) / CTL_ITEM_H;
}

/* ---- acting -------------------------------------------------------------- */

static void ctl_toggle(App *a, int which)
{
	/* The same tests the draw works from, in the same order: a box drawn dim
	 * has to be inert as well, or the tick appears on a control that then does
	 * nothing. */
	if (!ctl_check_enabled(a, which, ctl_size_count(a)))
		return;

	/* The two antialiasing boxes are one knob publicly -- they solve the
	 * same problem twice and the library takes one answer -- so each turns
	 * its own mode on, or off where it is the one in force, and ticking
	 * either unticks the other by the same token. Read off the knob itself,
	 * as the boxes are drawn from it. */
	if (which == CTL_CHECK_FSAA || which == CTL_CHECK_MLAA) {
		const FnlAntialias mode = which == CTL_CHECK_FSAA ? FNL_AA_MSAA
														  : FNL_AA_MLAA;
		fnl_set_antialias(a, fnl_antialias(a) == mode ? FNL_AA_NONE : mode);
		fnl_invalidate(a);
		return;
	}

	const bool now = !CTL_CHECKED(a, which);
	fnl_atomic_store_bool(&a->ctl_check[which], now);

	/* Ticking the scaling box with nothing picked yet takes the area's current
	 * size, so the tick alone is a complete instruction: it means "hold the
	 * scene at what I am looking at now". */
	if (which == CTL_CHECK_SCALE && now && CTL_PIN_W(a) <= 0) {
		fnl_mutex_lock(&a->lock);
		const int aw = a->pub_area_w_px, ah = a->pub_area_h_px;
		fnl_mutex_unlock(&a->lock);
		fnl_atomic_store_u32(&a->ctl_pin_w, (uint32_t)(aw > 0 ? aw : 0));
		fnl_atomic_store_u32(&a->ctl_pin_h, (uint32_t)(ah > 0 ? ah : 0));
	}

	if (which == CTL_CHECK_SCALE)
		fnl_set_scene_size_px(a, now ? CTL_PIN_W(a) : 0,
							  now ? CTL_PIN_H(a) : 0);

	/* The tick is state the frame comparison cannot see, and not every box
	 * changes something it can. */
	fnl_invalidate(a);
}

static void ctl_select(App *a, int win_w, int item)
{
	static FNL_THREAD_LOCAL CtlList l;
	if (!ctl_list(a, win_w, &l) || item < 0 || item >= l.n) {
		ctl_set_open(a, CTL_SHUT);
		return;
	}
	const CtlRes *opts = l.size;
	if (l.which == CTL_OPEN_OUTPUT) {
		fnl_request_output(a, l.name[item]);
	} else if (l.which == CTL_OPEN_GPU) {
		/* A row is its place in the list (ctl_gpus()), and the one after
		 * the GPUs the calibration. */
		if (item >= l.gpu_n)
			app_rank_recalibrate(a);
		else
			fnl_request_gpu(a, item);
	} else if (BACKEND(a)->is_output) {
		/* A mode, as the row names it. The pin is left alone -- it is the
		 * scene's size and not the output's -- so a scene held at the old
		 * mode's area stays held across the switch. */
		const FnlMode m = { opts[item].w, opts[item].h, opts[item].mhz };
		fnl_request_mode(a, &m);
	} else {
		fnl_atomic_store_u32(&a->ctl_pin_w, (uint32_t)opts[item].w);
		fnl_atomic_store_u32(&a->ctl_pin_h, (uint32_t)opts[item].h);

		/* Maximized or fullscreen, the size is remembered and nothing else;
		 * the checkbox is what spends it there. The request names the
		 * presentable area, which is what the list picks. */
		if (!a->is_maximized && !a->is_fullscreen) {
			const int s120 = ctl_env(a).s120;
			fnl_request_size_pt(a, res_window_w_pt(opts[item].w, s120),
								res_window_h_pt(opts[item].h, s120, 0));
		}
		if (CTL_CHECKED(a, CTL_CHECK_SCALE))
			fnl_set_scene_size_px(a, CTL_PIN_W(a), CTL_PIN_H(a));
	}
	ctl_set_open(a, CTL_SHUT);
}

/* A press that landed on a control, from deco_press()'s ZONE_CONTROL branch.
 * The row is ctl_popup_hit()'s answer, or -1 when the press was in the band. */
void ctl_press(App *a, int win_w, int x, int y, int row)
{
	if (row >= 0) {
		ctl_select(a, win_w, row);
		return;
	}

	const int what = ctl_hit(a, win_w, x, y);
	if (what < 0) {
		/* A press anywhere else puts an open list away: deco_hit() routes
		 * every press here while it is open (ctl_modal()). */
		if (CTL_OPEN(a))
			ctl_set_open(a, CTL_SHUT);
		return;
	}
	/* A box opens its list, shuts it, or swaps the other's for its own. */
	if (what == CTL_HIT_SIZE || what == CTL_HIT_OUTPUT ||
		what == CTL_HIT_GPU) {
		const int want = what == CTL_HIT_SIZE   ? CTL_OPEN_SIZE
					   : what == CTL_HIT_OUTPUT ? CTL_OPEN_OUTPUT
												: CTL_OPEN_GPU;
		const bool any = what == CTL_HIT_SIZE   ? ctl_size_count(a) > 0
					   : what == CTL_HIT_OUTPUT ? ctl_output_count(a) > 1
												: ctl_gpu_count(a) > 1;
		if (CTL_OPEN(a) == want)
			ctl_set_open(a, CTL_SHUT);
		else if (any)
			ctl_set_open(a, want);
		else if (CTL_OPEN(a))
			ctl_set_open(a, CTL_SHUT);
		return;
	}
	/* A checkbox pressed while a list is open only puts the list away,
	 * as a press anywhere else does. */
	if (CTL_OPEN(a)) {
		ctl_set_open(a, CTL_SHUT);
		return;
	}
	ctl_toggle(a, what);
}

/* The pointer moved. Only the open list has anything to follow, and only when
 * the row actually changed -- invalidating on every motion event would
 * re-record the frame for a highlight that has not moved. */
void ctl_motion(App *a, int win_w, int x, int y)
{
	const int row = ctl_popup_hit(a, win_w, x, y);
	if (row != CTL_HOVER(a)) {
		ctl_set_hover(a, row);
		fnl_invalidate(a);
	}
}
