/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The benchmark's two panels: the setup, where every option is a row worked
 * with the mouse, and the results of the last run. Drawn over the scene
 * with the overlay's primitives, and made the mouse's with fnl_claim(): the
 * panel claims its rectangle every frame, beside its drawing, so a press on
 * it is an event and takes no pointer, while the scene around the panel
 * still captures on a click as it always does.
 *
 * Every control has a key as well, which is how a harness works it: Tab the
 * page, Up and Down the row, Left and Right (and the wheel) the value,
 * Enter a text row's editing, F5 the start; on the results R, B and Q.
 *
 * The layout is computed once a frame, before the events, so the events and
 * the drawing read the same rectangles.
 */

#include "bench.h"
#include "demo.h"

#include <math.h>
#include <string.h>

static const FnlColor COL_PANEL  = DEMO_RGB(0.06f, 0.06f, 0.08f);
static const FnlColor COL_ROW    = DEMO_RGB(0.10f, 0.10f, 0.13f);
static const FnlColor COL_HOVER  = DEMO_RGB(0.17f, 0.18f, 0.24f);
static const FnlColor COL_SEL    = DEMO_RGB(0.22f, 0.24f, 0.32f);
static const FnlColor COL_BUTTON = DEMO_RGB(0.20f, 0.36f, 0.26f);
static const FnlColor COL_BTN_HI = DEMO_RGB(0.26f, 0.48f, 0.34f);
static const FnlColor COL_TITLE  = DEMO_RGB(1.00f, 0.78f, 0.30f);
static const FnlColor COL_KEY    = DEMO_RGB(0.80f, 0.85f, 0.95f);
static const FnlColor COL_DESC   = DEMO_RGB(0.55f, 0.58f, 0.65f);
static const FnlColor COL_VALUE  = DEMO_RGB(1.00f, 0.92f, 0.25f);
static const FnlColor COL_GOOD   = DEMO_RGB(0.35f, 1.00f, 0.55f);
static const FnlColor COL_BAD    = DEMO_RGB(1.00f, 0.38f, 0.35f);

#define PAD        12
#define ROW_GAP    2
#define PANEL_W    620
#define ROWS_MAX   16
#define BUTTONS    3

typedef struct { int x, y, w, h; } Rect;

typedef struct {
	Rect panel;
	Rect tabs[BENCH_PAGE_COUNT];
	int  ids[ROWS_MAX];                 /* the page's rows, as BenchOptId */
	Rect rows[ROWS_MAX];
	int  count;
	Rect buttons[BUTTONS];
	int  button_count;
	int  help_y;
} Layout;

static bool inside(const Rect *r, double x, double y)
{
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static int row_h(void)
{
	return fnl_text_line(FNL_TEXT_BODY) + 8;
}

/* The results' rows, each a name and a figure. */
#define RESULT_ROWS 15

static int longest_page(void)
{
	int most = 0;
	for (int pg = 0; pg < BENCH_PAGE_COUNT; pg++) {
		int n = 0;
		for (int i = 0; i < OPT_COUNT; i++)
			n += (int)BENCH_OPTIONS[i].page == pg;
		if (n > most)
			most = n;
	}
	return most < ROWS_MAX ? most : ROWS_MAX;
}

static void layout(FnlApp *app, const BenchPanel *p, Layout *l)
{
	memset(l, 0, sizeof *l);
	int cx, cy, cw, ch;
	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
	const int line = fnl_text_line(FNL_TEXT_BODY);
	const int rh = row_h();
	const int w = cw - 2 * PAD < PANEL_W ? cw - 2 * PAD : PANEL_W;
	const int x = cx + (cw - w) / 2;
	int y = cy + PAD;
	const int top = y;

	y += PAD + fnl_text_line(FNL_TEXT_LARGE) + PAD;      /* the title */
	if (!p->result) {
		const int tw = (w - 2 * PAD) / BENCH_PAGE_COUNT;
		for (int i = 0; i < BENCH_PAGE_COUNT; i++)
			l->tabs[i] = (Rect){ x + PAD + i * tw, y, tw - ROW_GAP, rh };
		y += rh + PAD;
		for (int i = 0; i < OPT_COUNT && l->count < ROWS_MAX; i++) {
			if ((int)BENCH_OPTIONS[i].page != p->page)
				continue;
			l->ids[l->count] = i;
			l->rows[l->count] = (Rect){ x + PAD, y + l->count * (rh + ROW_GAP),
										w - 2 * PAD, rh };
			l->count++;
		}
		/* As tall as the longest page, whichever is shown, so the help and
		 * the buttons stay where they are when the page changes. */
		y += longest_page() * (rh + ROW_GAP) + PAD;
		l->help_y = y;
		y += 2 * line + PAD;
		l->button_count = 2;
	} else {
		y += RESULT_ROWS * line + PAD;
		l->help_y = y;
		y += 3 * line + PAD;
		l->button_count = 3;
	}
	const int bw = (w - 2 * PAD - (l->button_count - 1) * PAD) /
				   l->button_count;
	for (int i = 0; i < l->button_count; i++)
		l->buttons[i] = (Rect){ x + PAD + i * (bw + PAD), y, bw, rh + 6 };
	y += rh + 6 + PAD;
	l->panel = (Rect){ x, top, w, y - top };
}

static const char *const SETUP_BUTTONS[] = { "Start  F5", "Quit  Esc" };
static const char *const RESULT_BUTTONS[] = {
	"Run again  R", "Setup  B", "Quit  Q",
};

/* ---- acting -------------------------------------------------------------- */

static void decide(BenchPanel *p, FnlApp *app, BenchAction a)
{
	p->action = a;
	fnl_quit(app);
}

static void press_button(BenchPanel *p, FnlApp *app, int i)
{
	if (!p->result)
		decide(p, app, i == 0 ? PANEL_START : PANEL_QUIT);
	else
		decide(p, app, i == 0 ? PANEL_RERUN : i == 1 ? PANEL_BACK
													 : PANEL_QUIT);
}

static void edit_begin(BenchPanel *p, FnlApp *app, int id)
{
	p->editing = true;
	snprintf(p->edit, sizeof p->edit, "%s",
			 bench_opt_text(p->config, (BenchOptId)id));
	fnl_set_text_input(app, true);
}

static void edit_end(BenchPanel *p, FnlApp *app, const Layout *l, bool keep)
{
	if (keep && p->row < l->count)
		bench_opt_set(p->config, (BenchOptId)l->ids[p->row], p->edit);
	p->editing = false;
	fnl_set_text_input(app, false);
}

/* Takes the last UTF-8 character off the edit. */
static void edit_back(BenchPanel *p)
{
	size_t n = strlen(p->edit);
	while (n > 0) {
		n--;
		if (((unsigned char)p->edit[n] & 0xc0) != 0x80)
			break;
	}
	p->edit[n] = '\0';
}

/* A row worked: a text row edited, any other stepped. */
static void work_row(BenchPanel *p, FnlApp *app, const Layout *l, int i,
					 int dir)
{
	p->row = i;
	const int id = l->ids[i];
	if (BENCH_OPTIONS[id].kind == BENCH_TEXT)
		edit_begin(p, app, id);
	else
		bench_opt_step(p->config, (BenchOptId)id, dir);
}

static void on_key(BenchPanel *p, FnlApp *app, const Layout *l,
				   const FnlKeyEvent *k)
{
	if (!k->pressed)
		return;
	if (p->editing) {
		if (k->key == FNL_KEY_ENTER)
			edit_end(p, app, l, true);
		else if (k->key == FNL_KEY_ESCAPE)
			edit_end(p, app, l, false);
		else if (k->key == FNL_KEY_BACKSPACE)
			edit_back(p);
		return;
	}
	if (k->repeat)
		return;
	if (p->result) {
		if (k->key == FNL_KEY_R || k->key == FNL_KEY_ENTER)
			decide(p, app, PANEL_RERUN);
		else if (k->key == FNL_KEY_B)
			decide(p, app, PANEL_BACK);
		else if (k->key == FNL_KEY_Q)
			decide(p, app, PANEL_QUIT);
		return;
	}
	switch (k->key) {
	case FNL_KEY_TAB:
		p->page = (p->page + 1) % BENCH_PAGE_COUNT;
		p->row = 0;
		break;
	case FNL_KEY_UP:
		if (p->row > 0)
			p->row--;
		break;
	case FNL_KEY_DOWN:
		if (p->row + 1 < l->count)
			p->row++;
		break;
	case FNL_KEY_LEFT:
	case FNL_KEY_RIGHT:
		if (p->row < l->count &&
			BENCH_OPTIONS[l->ids[p->row]].kind != BENCH_TEXT)
			bench_opt_step(p->config, (BenchOptId)l->ids[p->row],
						   k->key == FNL_KEY_RIGHT ? 1 : -1);
		break;
	case FNL_KEY_ENTER:
		if (p->row < l->count)
			work_row(p, app, l, p->row, 1);
		break;
	case FNL_KEY_F5:
		decide(p, app, PANEL_START);
		break;
	default:
		break;
	}
}

static void on_event(BenchPanel *p, FnlApp *app, const Layout *l,
					 const FnlEvent *ev)
{
	switch (ev->type) {
	case FNL_EVENT_MOTION: {
		int hover = -1, button = -1;
		for (int i = 0; i < l->count; i++)
			if (inside(&l->rows[i], ev->motion.x, ev->motion.y))
				hover = i;
		for (int i = 0; i < BENCH_PAGE_COUNT && !p->result; i++)
			if (inside(&l->tabs[i], ev->motion.x, ev->motion.y))
				hover = ROWS_MAX + i;
		for (int i = 0; i < l->button_count; i++)
			if (inside(&l->buttons[i], ev->motion.x, ev->motion.y))
				button = i;
		if (hover != p->hover || button != p->hover_button) {
			p->hover = hover;
			p->hover_button = button;
			fnl_set_cursor(app, hover >= 0 || button >= 0
								? FNL_CURSOR_POINTER : FNL_CURSOR_DEFAULT);
		}
		break;
	}
	case FNL_EVENT_BUTTON: {
		if (!ev->button.pressed || fnl_is_pointer_captured(app))
			break;
		const double x = ev->button.x, y = ev->button.y;
		const int dir = ev->button.button == FNL_BUTTON_RIGHT ? -1 : 1;
		if (p->editing)
			edit_end(p, app, l, true);
		for (int i = 0; i < l->button_count; i++)
			if (inside(&l->buttons[i], x, y) &&
				ev->button.button == FNL_BUTTON_LEFT)
				press_button(p, app, i);
		if (p->result)
			break;
		for (int i = 0; i < BENCH_PAGE_COUNT; i++)
			if (inside(&l->tabs[i], x, y)) {
				p->page = i;
				p->row = 0;
			}
		for (int i = 0; i < l->count; i++)
			if (inside(&l->rows[i], x, y))
				work_row(p, app, l, i, dir);
		break;
	}
	case FNL_EVENT_SCROLL:
		if (p->result || p->editing)
			break;
		for (int i = 0; i < l->count; i++) {
			const int id = l->ids[i];
			if (!inside(&l->rows[i], ev->scroll.x, ev->scroll.y) ||
				BENCH_OPTIONS[id].kind == BENCH_TEXT || ev->scroll.dy == 0.0)
				continue;
			p->row = i;
			bench_opt_step(p->config, (BenchOptId)id,
						   ev->scroll.dy < 0.0 ? 1 : -1);
		}
		break;
	case FNL_EVENT_KEY:
		on_key(p, app, l, &ev->key);
		break;
	case FNL_EVENT_TEXT:
		if (p->editing &&
			strlen(p->edit) + strlen(ev->text.utf8) < sizeof p->edit)
			strcat(p->edit, ev->text.utf8);
		break;
	default:
		break;
	}
}

/* ---- drawing ------------------------------------------------------------- */

static void text_at(FnlApp *app, int x, int y, FnlColor c, const char *s)
{
	fnl_overlay_text(app, x, y, FNL_TEXT_BODY, c, s);
}

/* The setup: tabs, the page's rows, the hovered or chosen row's help. */
static void draw_setup(BenchPanel *p, FnlApp *app, const Layout *l)
{
	const int line = fnl_text_line(FNL_TEXT_BODY);
	const int ty = (row_h() - line) / 2;
	for (int i = 0; i < BENCH_PAGE_COUNT; i++) {
		const Rect *r = &l->tabs[i];
		fnl_overlay_rect(app, r->x, r->y, r->w, r->h,
						 i == p->page ? COL_SEL : p->hover == ROWS_MAX + i
									? COL_HOVER : COL_ROW);
	}
	for (int i = 0; i < l->count; i++) {
		const Rect *r = &l->rows[i];
		fnl_overlay_rect(app, r->x, r->y, r->w, r->h,
						 i == p->row ? COL_SEL : i == p->hover ? COL_HOVER
															   : COL_ROW);
	}
	for (int i = 0; i < l->button_count; i++) {
		const Rect *r = &l->buttons[i];
		fnl_overlay_rect(app, r->x, r->y, r->w, r->h,
						 i == p->hover_button ? COL_BTN_HI : COL_BUTTON);
	}

	for (int i = 0; i < BENCH_PAGE_COUNT; i++) {
		const Rect *r = &l->tabs[i];
		const int tw = fnl_text_width(BENCH_PAGE_NAMES[i], FNL_TEXT_BODY);
		text_at(app, r->x + (r->w - tw) / 2, r->y + ty,
				i == p->page ? COL_TITLE : COL_KEY, BENCH_PAGE_NAMES[i]);
	}
	for (int i = 0; i < l->count; i++) {
		const Rect *r = &l->rows[i];
		const int id = l->ids[i];
		const BenchOption *o = &BENCH_OPTIONS[id];
		text_at(app, r->x + 8, r->y + ty, COL_KEY, o->label);
		char v[BENCH_TEXT_MAX + 8];
		if (p->editing && i == p->row)
			snprintf(v, sizeof v, "%s_", p->edit);
		else if (o->kind == BENCH_TEXT)
			snprintf(v, sizeof v, "%s",
					 bench_opt_text(p->config, (BenchOptId)id)[0]
						 ? bench_opt_text(p->config, (BenchOptId)id)
						 : "-");
		else {
			char f[BENCH_TEXT_MAX];
			bench_opt_format(p->config, (BenchOptId)id, f, sizeof f);
			snprintf(v, sizeof v, "<  %s  >", f);
		}
		/* Cut from the left to the room there is, so the end of a long
		 * path stays in sight. */
		const int room = r->w / 2 - 8;
		const char *shown = v;
		while (*shown && fnl_text_width(shown, FNL_TEXT_BODY) > room)
			shown++;
		text_at(app, r->x + r->w - 8 - fnl_text_width(shown, FNL_TEXT_BODY),
				r->y + ty, p->config->set[id] ? COL_VALUE : COL_DESC, shown);
	}
	const int focus = p->hover >= 0 && p->hover < l->count ? p->hover
														   : p->row;
	if (focus < l->count) {
		const BenchOption *o = &BENCH_OPTIONS[l->ids[focus]];
		char help[320];
		snprintf(help, sizeof help, "--bench-%s: %s", o->key, o->help);
		/* Two lines at most, broken at a space. */
		const int room = l->panel.w - 2 * PAD;
		char first[320];
		snprintf(first, sizeof first, "%s", help);
		const char *rest = "";
		while (fnl_text_width(first, FNL_TEXT_BODY) > room) {
			char *sp = strrchr(first, ' ');
			if (!sp)
				break;
			*sp = '\0';
			rest = help + (sp - first) + 1;
		}
		text_at(app, l->panel.x + PAD, l->help_y, COL_DESC, first);
		text_at(app, l->panel.x + PAD, l->help_y + line, COL_DESC, rest);
	}
}

static void figure(char *out, size_t n, double v, const char *unit)
{
	if (isfinite(v))
		snprintf(out, n, "%.2f %s", v, unit);
	else
		snprintf(out, n, "-");
}

static void draw_results(BenchPanel *p, FnlApp *app, const Layout *l)
{
	for (int i = 0; i < l->button_count; i++) {
		const Rect *r = &l->buttons[i];
		fnl_overlay_rect(app, r->x, r->y, r->w, r->h,
						 i == p->hover_button ? COL_BTN_HI : COL_BUTTON);
	}
	const BenchResult *r = p->result;
	const int line = fnl_text_line(FNL_TEXT_BODY);
	const int x = l->panel.x + PAD;
	const int vx = x + 18 * fnl_text_advance(FNL_TEXT_BODY);
	int y = l->panel.y + PAD + fnl_text_line(FNL_TEXT_LARGE) + PAD;

	const BenchDist iv = bench_dist(&r->interval_ms);
	const BenchDist gpu = bench_dist(&r->gpu_ms);
	const BenchDist cpu = bench_dist(&r->cpu_ms);
	const BenchDist rss = bench_dist(&r->rss_mib);
	const BenchDist vram = bench_dist(&r->vram_mib);
	const BenchDist in = bench_dist(&r->input_ms);
	char v[RESULT_ROWS][400];
	snprintf(v[0], sizeof v[0], "%s, run %d of %d", r->status,
			 r->repeat + 1, r->repeats);
	snprintf(v[1], sizeof v[1], "%s on %s, %s", r->renderer, r->backend,
			 r->gpu_short);
	snprintf(v[2], sizeof v[2], "%.1f s, %lld frames", r->measure_s,
			 r->frames);
	figure(v[3], sizeof v[3], r->measure_s > 0
							  ? (double)r->frames / r->measure_s : BENCH_NAN,
		   "fps");
	snprintf(v[4], sizeof v[4], "p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms",
			 iv.p50, iv.p95, iv.p99, iv.max);
	snprintf(v[5], sizeof v[5], "1%% %.1f  0.1%% %.1f fps",
			 bench_low_fps(&r->interval_ms, 0.01),
			 bench_low_fps(&r->interval_ms, 0.001));
	figure(v[6], sizeof v[6], gpu.mean, "ms");
	figure(v[7], sizeof v[7], cpu.mean, "ms");
	figure(v[8], sizeof v[8], r->cpu_pct_mean, "%");
	figure(v[9], sizeof v[9], rss.mean, "MiB");
	figure(v[10], sizeof v[10], vram.mean, "MiB");
	figure(v[11], sizeof v[11], in.mean, "ms");
	snprintf(v[12], sizeof v[12], "%s", r->present_mode);
	snprintf(v[13], sizeof v[13], "%dx%d px, scale %d%%%s", r->area_w_px,
			 r->area_h_px, r->scale120 * 100 / 120,
			 r->scene_motion ? ", scene moving" : "");
	/* The frame loop's own account, each phase's mean per frame. */
	const FnlFrameStats *s0 = &r->st0, *s1 = &r->st1;
	const uint64_t fn = s1->frames - s0->frames;
	if (r->stats && s1->frames > s0->frames)
		snprintf(v[14], sizeof v[14], "pace %.2f  wait %.2f  record %.2f  "
				 "present %.2f ms",
				 (double)(s1->pace_us - s0->pace_us) / 1e3 / (double)fn,
				 (double)(s1->fence_us - s0->fence_us + s1->acquire_us -
						  s0->acquire_us) / 1e3 / (double)fn,
				 (double)(s1->record_us - s0->record_us) / 1e3 / (double)fn,
				 (double)(s1->present_us - s0->present_us) / 1e3 /
					 (double)fn);
	else
		snprintf(v[14], sizeof v[14], "-");
	static const char *const NAMES[RESULT_ROWS] = {
		"Status", "Platform", "Measured", "Frame rate", "Interval",
		"Low", "GPU per frame", "CPU per frame", "Process CPU", "Resident",
		"GPU memory", "Input latency", "Present", "Area", "Frame loop",
	};
	for (int i = 0; i < RESULT_ROWS; i++, y += line) {
		text_at(app, x, y, COL_KEY, NAMES[i]);
		const FnlColor c = i == 0 ? (!strcmp(r->status, "complete")
									 ? COL_GOOD : COL_BAD) : COL_VALUE;
		text_at(app, vx, y, c, v[i]);
	}
	y = l->help_y;
	char t[640];
	if (r->invalid[0]) {
		snprintf(t, sizeof t, "Invalid: %s", r->invalid);
		text_at(app, x, y, COL_BAD, t);
	}
	y += line;
	if (r->notes[0]) {
		snprintf(t, sizeof t, "Notes: %s", r->notes);
		text_at(app, x, y, COL_DESC, t);
	}
	y += line;
	/* The file's name: the directory is the option's, or the state
	 * directory's, and the name is what tells two runs apart. */
	const char *path = p->written && p->written[0] ? p->written : "-";
	for (const char *s = path; *s; s++)
		if ((*s == '/' || *s == '\\') && s[1])
			path = s + 1;
	const int room = l->panel.w - 2 * PAD - fnl_text_width("Written ",
														   FNL_TEXT_BODY);
	while (*path && fnl_text_width(path, FNL_TEXT_BODY) > room)
		path++;
	snprintf(t, sizeof t, "Written %s", path);
	text_at(app, x, y, COL_DESC, t);
}

void bench_panel_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	(void)frame;
	BenchPanel *p = ud;
	Layout l;
	layout(app, p, &l);

	FnlEvent ev;
	bool any = false;
	while (fnl_poll_event(app, &ev)) {
		on_event(p, app, &l, &ev);
		any = true;
		/* A page change moves the rows. */
		layout(app, p, &l);
	}
	if (p->row >= l.count)
		p->row = l.count > 0 ? l.count - 1 : 0;
	/* The library cannot see this panel's state, so a frame after an event
	 * is asked for: the hover, a value, a page. */
	if (any)
		fnl_invalidate(app);

	/* While the pointer is captured the panel is out of reach: the scene
	 * has it until F9. */
	if (fnl_is_pointer_captured(app))
		return;

	fnl_overlay_rect(app, l.panel.x, l.panel.y, l.panel.w, l.panel.h,
					 COL_PANEL);
	if (p->result)
		draw_results(p, app, &l);
	else
		draw_setup(p, app, &l);

	/* In the body's face: the large one is the counter's, figures only.
	 * Centred in a band the large line's height, which the layout keeps. */
	const char *title = p->result ? "Benchmark results" : "Benchmark";
	fnl_overlay_text(app, l.panel.x + PAD,
					 l.panel.y + PAD + (fnl_text_line(FNL_TEXT_LARGE) -
										fnl_text_line(FNL_TEXT_BODY)) / 2,
					 FNL_TEXT_BODY, COL_TITLE, title);
	const char *const *names = p->result ? RESULT_BUTTONS : SETUP_BUTTONS;
	const int line = fnl_text_line(FNL_TEXT_BODY);
	for (int i = 0; i < l.button_count; i++) {
		const Rect *r = &l.buttons[i];
		const int tw = fnl_text_width(names[i], FNL_TEXT_BODY);
		fnl_overlay_text(app, r->x + (r->w - tw) / 2, r->y + (r->h - line) / 2,
						 FNL_TEXT_BODY, COL_KEY, names[i]);
	}

	/* The whole panel claimed, one rectangle: a press on it is the
	 * panel's, between the controls too, and the rows tell themselves
	 * apart from the layout. */
	fnl_claim(app, l.panel.x, l.panel.y, l.panel.w, l.panel.h);
}
