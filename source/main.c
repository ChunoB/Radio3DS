// Radio 3DS - spin the globe, center a city, listen to its radio stations.
#include "common.h"
#include "updater.h"
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum { TAB_NOW, TAB_HERE, TAB_FAVS, TAB_SEARCH, TAB_ALARM, TAB_SETTINGS, NTABS };
enum { MODE_NORMAL, MODE_CLOCK, MODE_DARK };
enum {
	BTN_TAB0 = 0, BTN_FAV = 10, BTN_PLAYSTOP, BTN_RANDOM, BTN_VOLDN, BTN_VOLUP,
	BTN_ALARM_TOGGLE, BTN_AH_DN, BTN_AH_UP, BTN_AM_DN, BTN_AM_UP, BTN_ASRC_PREV, BTN_ASRC_NEXT,
	BTN_SLEEP_PREV, BTN_SLEEP_NEXT, BTN_CLOCKMODE, BTN_TEST_ALARM, BTN_TZ_DN, BTN_TZ_UP,
	BTN_SEARCH_FIELD, BTN_RING_CONT, BTN_RING_OFF, BTN_RING_SNOOZE, BTN_DARKMODE, BTN_SPK_LID,
	BTN_UPDATE,
};

#define ROW_H 18
#define MAX_HITS 150

typedef struct { float x, y, w, h; int id; } Btn;
typedef struct { int kind, idx; } Hit;   // kind 0 = place, 1 = station

static C3D_RenderTarget *t_top, *t_top_r, *t_bot;

// ---- app state
static int   tab = TAB_NOW;
static int   sel[NTABS], top_row[NTABS];
static bool  list_active;
static float vlat, vlon, zoom = 1;
static bool  anim;
static float alat, alon;
static int   idle_frames;
static int   focus = -1;
static char  now_name[64], now_url[512];
static int   now_place = -1;
static int   mode = MODE_NORMAL;
static bool  audio_ok;
static bool  lid_closed, bl_top = true, bl_bot = true;
static bool  stereo_on;
static bool  sleep_allowed = true;

static bool  ringing;
static u64   ring_start, snooze_until;
static int   last_fired_yday = -1;
static const int sleep_opts[] = { 0, 15, 30, 45, 60, 90 };
static int   sleep_idx;
static u64   sleep_end;

static Hit   hits[MAX_HITS];
static int   nhits;
static char  query[64];

static char  toast_msg[96];
static u64   toast_until;
static float bands[32];
static float cur_vol = -1;

static Btn   btns[48], pbtns[48];
static int   nbtns, npbtns;
static int   held_btn = -1, held_frames;
static int   touch_start_y, touch_start_top;
static bool  touch_in_list, dragging;
static struct { int tab; float y0; int rows; } lst;

static u64   now_ms;
static time_t now_t;

// ------------------------------------------------------------ small helpers

static float wrap180(float a)
{
	while (a > 180) a -= 360;
	while (a < -180) a += 360;
	return a;
}

static void toast(const char *m)
{
	snprintf(toast_msg, sizeof toast_msg, "%s", m);
	toast_until = now_ms + 2200;
}

static int list_rows(int t)
{
	switch (t) {
	case TAB_HERE: return 8;
	case TAB_FAVS: return 9;
	case TAB_SEARCH: return 8;
	default: return 0;
	}
}

static int list_count(int t)
{
	switch (t) {
	case TAB_HERE: return focus >= 0 ? g_places[focus].count : 0;
	case TAB_FAVS: return g_nfavs;
	case TAB_SEARCH: return nhits;
	default: return 0;
	}
}

static void ensure_visible(int t)
{
	int rows = list_rows(t), n = list_count(t);
	if (sel[t] >= n) sel[t] = n - 1;
	if (sel[t] < 0) sel[t] = 0;
	if (sel[t] < top_row[t]) top_row[t] = sel[t];
	if (sel[t] >= top_row[t] + rows) top_row[t] = sel[t] - rows + 1;
	if (top_row[t] > n - rows) top_row[t] = n - rows;
	if (top_row[t] < 0) top_row[t] = 0;
}

static void fly_to(int place)
{
	if (place < 0 || place >= g_nplaces) return;
	anim = true;
	alat = g_places[place].lat;
	alon = g_places[place].lon;
}

static void play_url(const char *name, const char *url, int place)
{
	if (!audio_ok) { toast("Áudio indisponível (falta dspfirm.cdc)"); return; }
	if (name != now_name) snprintf(now_name, sizeof now_name, "%s", name);
	if (url != now_url) snprintf(now_url, sizeof now_url, "%s", url);
	now_place = place;
	player_play(now_url);
	snprintf(g_cfg.last_name, sizeof g_cfg.last_name, "%s", now_name);
	snprintf(g_cfg.last_url, sizeof g_cfg.last_url, "%s", now_url);
	g_cfg.last_place = place;
	if (place >= 0) fly_to(place);
}

static void play_station(int si)
{
	if (si < 0 || si >= g_nstations) return;
	const Station *s = &g_stations[si];
	play_url(s->name, s->url, s->place);
}

static void play_focus(void)
{
	if (focus < 0 || g_places[focus].count == 0) return;
	play_station(g_places[focus].first);
	tab = TAB_HERE;
	sel[TAB_HERE] = 0;
	top_row[TAB_HERE] = 0;
	list_active = true;
}

static void play_random(void)
{
	if (!g_nstations) return;
	play_station(rand() % g_nstations);
	toast("Surpresa!");
}

static void toggle_fav_current(void)
{
	if (!now_url[0]) return;
	toast(fav_toggle(now_name, now_url, now_place) ? "Adicionada aos favoritos" : "Removida dos favoritos");
}

static void play_or_stop(void)
{
	if (player_state() != PL_STOPPED) { player_stop(); return; }
	if (now_url[0]) play_url(now_name, now_url, now_place);
	else if (g_cfg.last_url[0]) play_url(g_cfg.last_name, g_cfg.last_url, g_cfg.last_place);
	else play_focus();
}

static void activate_row(int t, int i)
{
	if (t == TAB_HERE && focus >= 0 && i < g_places[focus].count) {
		play_station(g_places[focus].first + i);
	} else if (t == TAB_FAVS && i < g_nfavs) {
		Fav *f = &g_favs[i];
		play_url(f->name, f->url, f->place);
	} else if (t == TAB_SEARCH && i < nhits) {
		if (hits[i].kind == 0) {
			fly_to(hits[i].idx);
			tab = TAB_HERE;
			sel[TAB_HERE] = top_row[TAB_HERE] = 0;
			focus = hits[i].idx;
		} else {
			play_station(hits[i].idx);
		}
	}
}

static void fav_selected(void)
{
	int i = sel[tab];
	if (tab == TAB_HERE && focus >= 0 && i < g_places[focus].count) {
		const Station *s = &g_stations[g_places[focus].first + i];
		toast(fav_toggle(s->name, s->url, s->place) ? "Adicionada aos favoritos" : "Removida dos favoritos");
	} else if (tab == TAB_FAVS && i < g_nfavs) {
		Fav f = g_favs[i];
		fav_toggle(f.name, f.url, f.place);
		toast("Removida dos favoritos");
		ensure_visible(TAB_FAVS);
	} else if (tab == TAB_SEARCH && i < nhits && hits[i].kind == 1) {
		const Station *s = &g_stations[hits[i].idx];
		toast(fav_toggle(s->name, s->url, s->place) ? "Adicionada aos favoritos" : "Removida dos favoritos");
	} else {
		toggle_fav_current();
	}
}

static void do_search(void)
{
	SwkbdState kb;
	char buf[64];
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 60);
	swkbdSetHintText(&kb, "Cidade, rádio ou gênero (ex: jazz)");
	swkbdSetInitialText(&kb, query);
	if (swkbdInputText(&kb, buf, sizeof buf) != SWKBD_BUTTON_CONFIRM) return;
	snprintf(query, sizeof query, "%s", buf);

	char q[128], f[256];
	fold_text(query, q, sizeof q);
	nhits = 0;
	sel[TAB_SEARCH] = top_row[TAB_SEARCH] = 0;
	if (!q[0]) return;
	for (int i = 0; i < g_nplaces && nhits < MAX_HITS; i++) {
		fold_text(g_places[i].name, f, sizeof f);
		if (strstr(f, q)) hits[nhits++] = (Hit){ 0, i };
	}
	for (int i = 0; i < g_nstations && nhits < MAX_HITS; i++) {
		fold_text(g_stations[i].name, f, sizeof f);
		bool m = strstr(f, q) != NULL;
		if (!m) {
			fold_text(g_stations[i].tags, f, sizeof f);
			m = strstr(f, q) != NULL;
		}
		if (m) hits[nhits++] = (Hit){ 1, i };
	}
	list_active = true;
	if (!nhits) toast("Nada encontrado");
}

// ------------------------------------------------------------ alarm

static void start_alarm(void)
{
	ringing = true;
	ring_start = now_ms;
	const char *name = NULL, *url = NULL;
	int place = -1;
	if (g_cfg.alarm_src >= 0 && g_cfg.alarm_src < g_nfavs) {
		Fav *f = &g_favs[g_cfg.alarm_src];
		name = f->name; url = f->url; place = f->place;
	} else if (g_cfg.last_url[0]) {
		name = g_cfg.last_name; url = g_cfg.last_url; place = g_cfg.last_place;
	}
	if (url) play_url(name, url, place);
	else player_tone(true);
}

static void ring_continue(void)
{
	ringing = false;
	player_tone(false);
}

static void ring_off(void)
{
	ringing = false;
	player_tone(false);
	player_stop();
}

static void ring_snooze(void)
{
	ring_off();
	snooze_until = now_ms + 9 * 60 * 1000;
	toast("Soneca: toca de novo em 9 min");
}

static void update_alarm(void)
{
	struct tm lt;
	gmtime_r(&now_t, &lt);
	if (g_cfg.alarm_on && !ringing && lt.tm_hour == g_cfg.alarm_h && lt.tm_min == g_cfg.alarm_m &&
	    lt.tm_yday != last_fired_yday) {
		last_fired_yday = lt.tm_yday;
		snooze_until = 0;
		start_alarm();
	}
	if (snooze_until && now_ms >= snooze_until) {
		snooze_until = 0;
		start_alarm();
	}
	if (ringing) {
		PlayerState st = player_state();
		u64 el = now_ms - ring_start;
		if (st == PL_PLAYING) player_tone(false);
		else if (el > 20000 || st == PL_ERROR || st == PL_STOPPED) player_tone(true);
		if (el > 60ULL * 60 * 1000) ring_continue();   // give up ringing after an hour
	}
	if (sleep_end && now_ms >= sleep_end) {
		sleep_end = 0;
		sleep_idx = 0;
		if (!ringing) player_stop();
	}

	// effective volume: user setting, alarm ramp, sleep fade-out
	float v = g_cfg.volume / 100.0f;
	if (ringing) {
		float ramp = 0.15f + 0.85f * (now_ms - ring_start) / 60000.0f;
		if (ramp > 1) ramp = 1;
		v = (v < 0.5f ? 0.5f : v) * ramp;
	} else if (sleep_end && sleep_end - now_ms < 30000) {
		v *= (sleep_end - now_ms) / 30000.0f;
	}
	if (fabsf(v - cur_vol) > 0.002f) {
		cur_vol = v;
		player_set_volume(v);
	}
}

// ------------------------------------------------------------ screens / power

static void set_backlight(u32 screen, bool on, bool *state)
{
	if (on == *state) return;
	if (on) GSPLCD_PowerOnBacklight(screen);
	else GSPLCD_PowerOffBacklight(screen);
	*state = on;
}

static void update_power(void)
{
	static int tick;
	if (++tick % 15 == 0) {
		u8 shell = 1;
		if (R_SUCCEEDED(PTMU_GetShellState(&shell))) lid_closed = shell == 0;
	}
	bool top_on = !lid_closed && (mode != MODE_DARK || ringing);
	bool bot_on = !lid_closed && (mode == MODE_NORMAL || ringing);
	set_backlight(GSPLCD_SCREEN_TOP, top_on, &bl_top);
	set_backlight(GSPLCD_SCREEN_BOTTOM, bot_on, &bl_bot);

	// keep running with the lid closed while something needs us
	bool busy = g_cfg.alarm_on || snooze_until || ringing || mode != MODE_NORMAL || player_state() != PL_STOPPED;
	// When the console may sleep, audio is shut down first: putting a running NDSP to
	// sleep can hang the system. It comes back as soon as something needs it.
	if (audio_ok) {
		if (busy && !player_audio_on()) {
			if (player_resume()) cur_vol = -1;
		} else if (!busy && player_audio_on()) {
			player_suspend();
		}
	}
	if (busy == sleep_allowed) {
		sleep_allowed = !busy;
		aptSetSleepAllowed(sleep_allowed);
	}

	speaker_update(lid_closed, ringing || player_state() != PL_STOPPED, now_ms);
}

static void apt_cb(APT_HookType hook, void *param)
{
	if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONEXIT) {
		GSPLCD_PowerOnAllBacklights();
		bl_top = bl_bot = true;
	}
}

// HOME is handled here instead of inside aptMainLoop: letting libctru suspend a running
// NDSP can freeze the console, so audio is shut down first and restarted on the way back.
static void go_home(void)
{
	bool was_playing = player_state() != PL_STOPPED && now_url[0];
	bool tone = player_suspend();
	aptJumpToHomeMenu();
	if (aptShouldClose()) return;   // closed from the HOME menu
	if (player_resume()) {
		cur_vol = -1;                // volume is applied again on the next frame
		if (was_playing) player_play(now_url);
		if (tone) player_tone(true);
	} else {
		toast("Som indisponível depois do HOME");
	}
}

// ------------------------------------------------------------ drawing helpers (bottom)

#define BLACK   C2D_Color32(0, 0, 0, 255)
#define WHITE   C2D_Color32(255, 255, 255, 255)
#define GRAY    C2D_Color32(110, 110, 120, 255)
#define LGRAY   C2D_Color32(200, 200, 208, 255)
#define BLUE    C2D_Color32(30, 50, 190, 255)
#define SELBG   C2D_Color32(40, 55, 160, 255)
#define RED     C2D_Color32(200, 30, 30, 255)

static void box(float x, float y, float w, float h, u32 fill, u32 border)
{
	C2D_DrawRectSolid(x, y, 0.5f, w, h, border);
	C2D_DrawRectSolid(x + 1, y + 1, 0.5f, w - 2, h - 2, fill);
}

static void reg_btn(float x, float y, float w, float h, int id)
{
	if (nbtns < 48) btns[nbtns++] = (Btn){ x, y, w, h, id };
}

static void button(float x, float y, float w, float h, const char *label, int id, bool on)
{
	bool pressed = held_btn == id;
	C2D_DrawRectSolid(x + 1, y + 1, 0.5f, w, h, BLACK);   // drop shadow
	box(x, y, w, h, pressed || on ? C2D_Color32(30, 30, 40, 255) : WHITE, BLACK);
	text_draw(label, x + w / 2, y + (h - 13) / 2, 0.45f, pressed || on ? WHITE : BLACK, w - 6, 1);
	reg_btn(x, y, w, h, id);
}

static void ring_b(float cx, float cy, float r, float th, u32 c)
{
	for (int i = 0; i < 16; i++) {
		float a0 = i * (float)M_PI / 8, a1 = (i + 1) * (float)M_PI / 8;
		C2D_DrawLine(cx + cosf(a0) * r, cy + sinf(a0) * r, c, cx + cosf(a1) * r, cy + sinf(a1) * r, c, th, 0.5f);
	}
}

static void icon(int which, float cx, float cy, u32 c, u32 bg)
{
	if (which == TAB_SEARCH) {
		ring_b(cx - 2, cy - 2, 4.5f, 1.6f, c);
		C2D_DrawLine(cx + 1.5f, cy + 1.5f, c, cx + 6, cy + 6, c, 2.2f, 0.5f);
	} else if (which == TAB_ALARM) {
		ring_b(cx, cy, 6, 1.5f, c);
		C2D_DrawLine(cx, cy, c, cx, cy - 4, c, 1.4f, 0.5f);
		C2D_DrawLine(cx, cy, c, cx + 3, cy, c, 1.4f, 0.5f);
		C2D_DrawLine(cx - 6, cy - 7, c, cx - 3, cy - 8, c, 1.6f, 0.5f);
		C2D_DrawLine(cx + 6, cy - 7, c, cx + 3, cy - 8, c, 1.6f, 0.5f);
	} else {
		for (int i = 0; i < 8; i++) {
			float a = i * (float)M_PI / 4;
			C2D_DrawRectSolid(cx + cosf(a) * 6 - 1.5f, cy + sinf(a) * 6 - 1.5f, 0.5f, 3, 3, c);
		}
		C2D_DrawCircleSolid(cx, cy, 0.5f, 5, c);
		C2D_DrawCircleSolid(cx, cy, 0.5f, 2, bg);
	}
}

static void play_triangle(float x, float y, u32 c)
{
	C2D_DrawTriangle(x, y, c, x, y + 9, c, x + 7, y + 4.5f, c, 0.5f);
}

static void draw_menubar(void)
{
	C2D_DrawRectSolid(0, 0, 0.5f, 320, 19, WHITE);
	C2D_DrawRectSolid(0, 19, 0.5f, 320, 1, BLACK);
	static const char *names[3] = { "Tocando", "Aqui", "Favoritos" };
	float x = 4;
	for (int i = 0; i < 3; i++) {
		float w = text_width(names[i], 0.47f) + 10;
		bool on = tab == i;
		if (on) C2D_DrawRectSolid(x, 0, 0.5f, w, 19, BLACK);
		text_draw(names[i], x + 5, 2, 0.47f, on ? WHITE : BLACK, 0, 0);
		reg_btn(x, 0, w, 20, BTN_TAB0 + i);
		x += w + 2;
	}
	for (int i = 0; i < 3; i++) {
		int t = TAB_SEARCH + i;
		float ix = 320 - 3 * 26 + i * 26;
		bool on = tab == t;
		if (on) C2D_DrawRectSolid(ix, 0, 0.5f, 26, 19, BLACK);
		icon(t, ix + 13, 10, on ? WHITE : BLACK, on ? BLACK : WHITE);
		reg_btn(ix, 0, 26, 20, BTN_TAB0 + t);
	}
}

static void draw_window(const char *title)
{
	C2D_DrawRectSolid(4, 23, 0.5f, 314, 217, C2D_Color32(0, 0, 0, 90));   // shadow
	box(3, 21, 314, 219, WHITE, BLACK);
	for (int i = 0; i < 6; i++) C2D_DrawRectSolid(5, 24 + i * 2, 0.5f, 310, 1, C2D_Color32(120, 120, 130, 255));
	box(9, 23, 11, 11, WHITE, BLACK);
	float tw = text_width(title, 0.47f) + 14;
	C2D_DrawRectSolid(160 - tw / 2, 22, 0.5f, tw, 13, WHITE);
	text_draw(title, 160, 22, 0.47f, BLACK, 0, 1);
	C2D_DrawRectSolid(4, 36, 0.5f, 312, 1, BLACK);
}

static void fmt_hm_t(char *out, size_t n, time_t t)
{
	struct tm tmv;
	gmtime_r(&t, &tmv);
	snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

static time_t utc_now(void) { return now_t - g_cfg.tz * 60; }

static void draw_list(int t, float y0, int count, void (*row)(int, float, float, bool))
{
	int rows = list_rows(t);
	lst.tab = t; lst.y0 = y0; lst.rows = rows;
	float w = count > rows ? 296 : 308;
	for (int r = 0; r < rows; r++) {
		int i = top_row[t] + r;
		if (i >= count) break;
		float y = y0 + r * ROW_H;
		bool s = list_active && i == sel[t];
		if (s) C2D_DrawRectSolid(6, y, 0.5f, w, ROW_H, SELBG);
		else if (r & 1) C2D_DrawRectSolid(6, y, 0.5f, w, ROW_H, C2D_Color32(240, 242, 250, 255));
		row(i, 8, y, s);
	}
	if (count > rows) {
		float h = rows * ROW_H;
		box(304, y0, 11, h, C2D_Color32(225, 225, 232, 255), BLACK);
		float th = h * rows / count;
		if (th < 12) th = 12;
		float ty = y0 + (h - th) * top_row[t] / (float)(count - rows);
		box(305, ty, 9, th, WHITE, BLACK);
	}
}

static void row_here(int i, float x, float y, bool s)
{
	const Station *st = &g_stations[g_places[focus].first + i];
	u32 c = s ? WHITE : BLACK;
	float nx = x + 2;
	if (!strcmp(st->url, now_url) && player_state() != PL_STOPPED) {
		play_triangle(x + 1, y + 4.5f, s ? WHITE : RED);
		nx = x + 12;
	}
	text_draw(st->name, nx, y + 2, 0.45f, c, 238 - (nx - x), 0);
	char r[24] = "";
	if (st->bitrate) snprintf(r, sizeof r, "%dk", st->bitrate);
	if (fav_find(st->url) >= 0) C2D_DrawCircleSolid(x + 262, y + 9, 0.5f, 3, s ? WHITE : C2D_Color32(230, 60, 120, 255));
	text_draw(r, x + 292, y + 3, 0.4f, s ? LGRAY : GRAY, 0, 2);
}

static void row_fav(int i, float x, float y, bool s)
{
	const Fav *f = &g_favs[i];
	u32 c = s ? WHITE : BLACK;
	float nx = x + 2;
	if (!strcmp(f->url, now_url) && player_state() != PL_STOPPED) {
		play_triangle(x + 1, y + 4.5f, s ? WHITE : RED);
		nx = x + 12;
	}
	text_draw(f->name, nx, y + 2, 0.45f, c, 190 - (nx - x), 0);
	if (f->place >= 0) text_draw(g_places[f->place].name, x + 292, y + 3, 0.4f, s ? LGRAY : GRAY, 95, 2);
	if (g_cfg.alarm_src == i) icon(TAB_ALARM, x + 200, y + 10, s ? WHITE : GRAY, s ? SELBG : WHITE);
}

static void row_hit(int i, float x, float y, bool s)
{
	u32 c = s ? WHITE : BLACK, c2 = s ? LGRAY : GRAY;
	if (hits[i].kind == 0) {
		const Place *p = &g_places[hits[i].idx];
		ring_b(x + 5, y + 9, 4, 1.4f, s ? WHITE : BLUE);
		text_draw(p->name, x + 14, y + 2, 0.45f, s ? WHITE : BLUE, 170, 0);
		char b[64];
		snprintf(b, sizeof b, "%s (%d)", p->country, p->count);
		text_draw(b, x + 292, y + 3, 0.4f, c2, 110, 2);
	} else {
		const Station *st = &g_stations[hits[i].idx];
		text_draw(st->name, x + 2, y + 2, 0.45f, c, 180, 0);
		if (st->place >= 0) text_draw(g_places[st->place].name, x + 292, y + 3, 0.4f, c2, 105, 2);
	}
}

static const char *state_text(PlayerState st)
{
	switch (st) {
	case PL_CONNECTING: return "Conectando...";
	case PL_BUFFERING: return "Carregando...";
	default: return "";
	}
}

static void draw_tab_now(void)
{
	PlayerState st = player_state();
	if (!now_url[0]) {
		text_draw("Nada tocando ainda", 160, 50, 0.55f, BLACK, 300, 1);
		text_draw("Gire o globo com o direcional circular,", 160, 80, 0.45f, GRAY, 300, 1);
		text_draw("centralize uma cidade e aperte A.", 160, 96, 0.45f, GRAY, 300, 1);
		text_draw("Ou aperte Y para uma rádio surpresa.", 160, 120, 0.45f, GRAY, 300, 1);
		if (!audio_ok) {
			text_draw("Áudio indisponível: rode o DSP1 para criar", 160, 150, 0.42f, RED, 300, 1);
			text_draw("sdmc:/3ds/dspfirm.cdc e abra o app de novo.", 160, 165, 0.42f, RED, 300, 1);
		}
		button(110, 182, 100, 22, "Surpresa", BTN_RANDOM, false);
		return;
	}
	text_draw(now_name, 12, 41, 0.55f, BLACK, 296, 0);
	text_draw(now_name, 12.6f, 41, 0.55f, BLACK, 296, 0);   // faux bold

	char info[160] = "";
	if (now_place >= 0) {
		const Place *p = &g_places[now_place];
		char hm[16];
		time_t u = utc_now();
		fmt_hm_t(hm, sizeof hm, u + place_utc_offset(p, u) * 60);
		snprintf(info, sizeof info, "%s, %s · %s", p->name, p->country, hm);
	}
	int kb = player_kbps();
	if (kb > 0) {
		size_t l = strlen(info);
		snprintf(info + l, sizeof info - l, "%s%d kbps", info[0] ? " · " : "", kb);
	}
	text_draw(info, 12, 60, 0.42f, GRAY, 296, 0);

	if (st == PL_ERROR) {
		char err[96];
		player_error(err, sizeof err);
		text_draw(err, 12, 78, 0.45f, RED, 296, 0);
	} else if (st == PL_CONNECTING || st == PL_BUFFERING) {
		text_draw(state_text(st), 12, 78, 0.45f, BLUE, 296, 0);
	} else if (st == PL_PLAYING) {
		char title[256], l1[256], l2[256];
		player_title(title, sizeof title);
		if (!title[0]) snprintf(title, sizeof title, "Ao vivo");
		text_wrap2(title, 0.45f, 296, l1, l2, sizeof l1);
		text_draw(l1, 12, 76, 0.45f, BLUE, 296, 0);
		text_draw(l2, 12, 91, 0.45f, BLUE, 296, 0);
	} else {
		text_draw("Parado", 12, 78, 0.45f, GRAY, 296, 0);
	}

	// spectrum
	box(12, 110, 296, 60, C2D_Color32(236, 238, 246, 255), BLACK);
	static const u8 stops[4][3] = { { 40, 120, 230 }, { 160, 90, 170 }, { 245, 150, 40 }, { 60, 175, 90 } };
	const int nb = 32;
	float bw = 288.0f / nb;
	for (int b = 0; b < nb; b++) {
		float t = b / (float)(nb - 1) * 3;
		int k = (int)t;
		if (k > 2) k = 2;
		float f = t - k;
		u32 c = C2D_Color32(stops[k][0] + (stops[k + 1][0] - stops[k][0]) * f,
		                    stops[k][1] + (stops[k + 1][1] - stops[k][1]) * f,
		                    stops[k][2] + (stops[k + 1][2] - stops[k][2]) * f, 255);
		float h = 2 + bands[b] * 52;
		C2D_DrawRectSolid(16 + b * bw, 167 - h, 0.5f, bw - 2, h, c);
	}

	bool fav = fav_find(now_url) >= 0;
	button(12, 180, 96, 24, fav ? "Favorita" : "Favoritar", BTN_FAV, fav);
	button(116, 180, 88, 24, st == PL_STOPPED ? "Tocar" : "Parar", BTN_PLAYSTOP, false);
	button(212, 180, 96, 24, "Surpresa", BTN_RANDOM, false);
}

static void draw_tab_here(void)
{
	if (focus < 0) return;
	const Place *p = &g_places[focus];
	char hm[16], b[160];
	time_t u = utc_now();
	fmt_hm_t(hm, sizeof hm, u + place_utc_offset(p, u) * 60);
	text_draw(p->name, 10, 39, 0.5f, BLACK, 200, 0);
	snprintf(b, sizeof b, "%s · %s", p->country, hm);
	text_draw(b, 310, 41, 0.42f, GRAY, 120, 2);
	C2D_DrawRectSolid(6, 56, 0.5f, 308, 1, LGRAY);
	draw_list(TAB_HERE, 58, p->count, row_here);
}

static void draw_tab_favs(void)
{
	if (!g_nfavs) {
		text_draw("Nenhum favorito ainda.", 160, 60, 0.5f, BLACK, 300, 1);
		text_draw("Aperte X em uma rádio para favoritar.", 160, 86, 0.45f, GRAY, 300, 1);
		text_draw("Rádios próprias: edite o arquivo", 160, 120, 0.42f, GRAY, 300, 1);
		text_draw("sdmc:/3ds/Radio3DS/favorites.tsv", 160, 136, 0.42f, GRAY, 300, 1);
		text_draw("(nome <TAB> url <TAB> -1)", 160, 152, 0.42f, GRAY, 300, 1);
		return;
	}
	draw_list(TAB_FAVS, 40, g_nfavs, row_fav);
}

static void draw_tab_search(void)
{
	box(8, 40, 304, 22, WHITE, BLACK);
	icon(TAB_SEARCH, 21, 51, GRAY, WHITE);
	if (query[0]) text_draw(query, 34, 44, 0.47f, BLACK, 270, 0);
	else text_draw("Toque para buscar cidade, rádio ou gênero", 34, 45, 0.42f, GRAY, 270, 0);
	reg_btn(8, 40, 304, 22, BTN_SEARCH_FIELD);
	if (nhits) draw_list(TAB_SEARCH, 66, nhits, row_hit);
	else if (query[0]) text_draw("Nada encontrado.", 160, 100, 0.45f, GRAY, 300, 1);
	else {
		text_draw("A ou Y: abrir o teclado", 160, 100, 0.45f, GRAY, 300, 1);
		text_draw("Cidades aparecem em azul.", 160, 118, 0.45f, GRAY, 300, 1);
	}
}

static void draw_tab_alarm(void)
{
	char b[96];
	text_draw("Alarme", 12, 46, 0.5f, BLACK, 0, 0);
	button(196, 42, 110, 22, g_cfg.alarm_on ? "Ligado" : "Desligado", BTN_ALARM_TOGGLE, g_cfg.alarm_on);

	text_draw("Horário", 12, 74, 0.5f, BLACK, 0, 0);
	button(128, 70, 22, 22, "-", BTN_AH_DN, false);
	snprintf(b, sizeof b, "%02d", g_cfg.alarm_h);
	text_draw(b, 165, 72, 0.6f, BLACK, 0, 1);
	button(180, 70, 22, 22, "+", BTN_AH_UP, false);
	text_draw(":", 212, 72, 0.6f, BLACK, 0, 1);
	button(222, 70, 22, 22, "-", BTN_AM_DN, false);
	snprintf(b, sizeof b, "%02d", g_cfg.alarm_m);
	text_draw(b, 259, 72, 0.6f, BLACK, 0, 1);
	button(274, 70, 22, 22, "+", BTN_AM_UP, false);

	text_draw("Tocar", 12, 102, 0.5f, BLACK, 0, 0);
	button(80, 98, 22, 22, "<", BTN_ASRC_PREV, false);
	const char *src = g_cfg.alarm_src >= 0 && g_cfg.alarm_src < g_nfavs ? g_favs[g_cfg.alarm_src].name
	                  : g_cfg.last_url[0] ? "Última rádio ouvida" : "Só o bipe";
	text_draw(src, 193, 102, 0.45f, BLUE, 160, 1);
	button(284, 98, 22, 22, ">", BTN_ASRC_NEXT, false);

	text_draw("Dormir em", 12, 130, 0.5f, BLACK, 0, 0);
	button(100, 126, 22, 22, "<", BTN_SLEEP_PREV, false);
	if (sleep_end) {
		int left = (int)((sleep_end - now_ms) / 60000) + 1;
		snprintf(b, sizeof b, "%d min (faltam %d)", sleep_opts[sleep_idx], left);
	} else snprintf(b, sizeof b, "Desligado");
	text_draw(b, 203, 130, 0.45f, BLUE, 150, 1);
	button(284, 126, 22, 22, ">", BTN_SLEEP_NEXT, false);

	button(12, 156, 120, 22, "Modo relógio", BTN_CLOCKMODE, false);
	button(140, 156, 82, 22, "Apagar", BTN_DARKMODE, false);
	button(230, 156, 76, 22, "Testar", BTN_TEST_ALARM, false);

	text_draw("Som com a tampa fechada", 12, 183, 0.42f, BLACK, 190, 0);
	text_draw(speaker_status(), 12, 196, 0.36f, GRAY, 190, 0);
	button(214, 182, 92, 22, g_cfg.spk_lid ? "Tentar" : "Desligado", BTN_SPK_LID, g_cfg.spk_lid);
}

static void draw_tab_settings(void)
{
	char b[96];
	text_draw("Seu fuso", 12, 44, 0.5f, BLACK, 0, 0);
	button(150, 40, 22, 22, "-", BTN_TZ_DN, false);
	int tz = g_cfg.tz, a = tz < 0 ? -tz : tz;
	snprintf(b, sizeof b, "UTC%c%02d:%02d", tz < 0 ? '-' : '+', a / 60, a % 60);
	text_draw(b, 218, 44, 0.5f, BLUE, 0, 1);
	button(264, 40, 22, 22, "+", BTN_TZ_UP, false);
	char hm[16], hu[16];
	fmt_hm_t(hm, sizeof hm, now_t);
	fmt_hm_t(hu, sizeof hu, utc_now());
	snprintf(b, sizeof b, "Relógio do 3DS: %s  ·  UTC: %s", hm, hu);
	text_draw(b, 12, 66, 0.42f, GRAY, 296, 0);
	C2D_DrawRectSolid(6, 82, 0.5f, 308, 1, LGRAY);

	static const char *help[] = {
		"Direcional circular: girar o globo   L / R: zoom",
		"Direcional: cima/baixo lista ou volume, lados abas",
		"A: ouvir   B: parar   X: favorito   Y: surpresa",
		"SELECT: modo relógio, depois telas apagadas",
		"Controle 3D: globo e relógio em 3D",
	};
	for (int i = 0; i < 5; i++) text_draw(help[i], 12, 86 + i * 14, 0.42f, BLACK, 300, 0);
	C2D_DrawRectSolid(6, 158, 0.5f, 308, 1, LGRAY);

	// updates (GitHub releases)
	UpdState us = updater_state();
	const char *msg = us == UPD_IDLE ? NULL : updater_message();
	snprintf(b, sizeof b, "Versão %s (%s)", updater_version(), updater_is_cia() ? "CIA" : "3DSX");
	text_draw(b, 12, 163, 0.45f, BLACK, 196, 0);
	if (msg) text_draw(msg, 12, 178, 0.38f, us == UPD_ERROR ? C2D_Color32(190, 30, 30, 255) : GRAY, 196, 0);
	if (us == UPD_DOWNLOADING || us == UPD_INSTALLING) {
		float p = updater_progress();
		C2D_DrawRectSolid(12, 192, 0.5f, 196, 5, LGRAY);
		C2D_DrawRectSolid(12, 192, 0.5f, 196 * (p < 0 ? 0 : p > 1 ? 1 : p), 5, BLUE);
	}
	const char *lbl = us == UPD_AVAILABLE ? "Instalar" : updater_busy() ? "Aguarde..." : "Atualizar";
	button(216, 164, 92, 22, lbl, BTN_UPDATE, us == UPD_AVAILABLE);

	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);
	snprintf(b, sizeof b, "%d rádios em %d cidades · %s", g_nstations, g_nplaces, n3ds ? "New 3DS" : "Old 3DS");
	text_draw(b, 160, 199, 0.38f, GRAY, 304, 1);
}

static void draw_status(void)
{
	C2D_DrawRectSolid(4, 211, 0.5f, 312, 1, BLACK);
	PlayerState st = player_state();
	char l1[128], l2[256];
	if (now_url[0] && st != PL_STOPPED) {
		snprintf(l1, sizeof l1, "No ar: %s", now_name);
		if (st == PL_PLAYING) player_title(l2, sizeof l2);
		else if (st == PL_ERROR) player_error(l2, sizeof l2);
		else snprintf(l2, sizeof l2, "%s", state_text(st));
	} else {
		snprintf(l1, sizeof l1, "Parado");
		l2[0] = 0;
		if (sleep_end || g_cfg.alarm_on) {
			if (g_cfg.alarm_on) snprintf(l2, sizeof l2, "Alarme %02d:%02d", g_cfg.alarm_h, g_cfg.alarm_m);
		}
	}
	text_draw(l1, 8, 212, 0.42f, BLACK, 214, 0);
	text_draw(l2, 8, 225, 0.4f, BLUE, 214, 0);

	button(226, 216, 18, 18, "-", BTN_VOLDN, false);
	char v[16];
	snprintf(v, sizeof v, "%d%%", g_cfg.volume);
	text_draw(v, 270, 218, 0.45f, BLACK, 0, 1);
	button(294, 216, 18, 18, "+", BTN_VOLUP, false);
}

static void draw_ring_overlay(void)
{
	nbtns = 0;   // only the alarm buttons are active
	C2D_DrawRectSolid(0, 0, 0.5f, 320, 240, C2D_Color32(0, 0, 0, 120));
	box(20, 30, 280, 180, WHITE, BLACK);
	for (int i = 0; i < 6; i++) C2D_DrawRectSolid(22, 33 + i * 2, 0.5f, 276, 1, C2D_Color32(120, 120, 130, 255));
	C2D_DrawRectSolid(130, 31, 0.5f, 60, 13, WHITE);
	text_draw("Alarme", 160, 31, 0.47f, BLACK, 0, 1);
	char hm[16];
	fmt_hm_t(hm, sizeof hm, now_t);
	text_draw(hm, 160, 52, 1.0f, BLACK, 0, 1);
	text_draw(now_url[0] ? now_name : "Bipe", 160, 88, 0.45f, BLUE, 260, 1);
	button(36, 112, 248, 26, "Continuar ouvindo (A)", BTN_RING_CONT, false);
	button(36, 144, 248, 26, "Soneca 9 min (X)", BTN_RING_SNOOZE, false);
	button(36, 176, 248, 26, "Desligar (B)", BTN_RING_OFF, false);
}

static void draw_bottom(void)
{
	C2D_TargetClear(t_bot, WHITE);
	C2D_SceneBegin(t_bot);
	nbtns = 0;
	C2D_DrawRectangle(0, 0, 0.5f, 320, 240, C2D_Color32(196, 206, 245, 255), C2D_Color32(226, 204, 240, 255),
	                  C2D_Color32(188, 226, 238, 255), C2D_Color32(214, 214, 250, 255));
	draw_menubar();
	static const char *titles[NTABS] = { "Tocando", "Aqui", "Favoritos", "Buscar", "Alarme e sono", "Ajustes" };
	draw_window(titles[tab]);
	switch (tab) {
	case TAB_NOW: draw_tab_now(); break;
	case TAB_HERE: draw_tab_here(); break;
	case TAB_FAVS: draw_tab_favs(); break;
	case TAB_SEARCH: draw_tab_search(); break;
	case TAB_ALARM: draw_tab_alarm(); break;
	case TAB_SETTINGS: draw_tab_settings(); break;
	}
	draw_status();
	if (toast_until > now_ms) {
		float w = text_width(toast_msg, 0.45f) + 20;
		box(160 - w / 2, 186, w, 20, C2D_Color32(30, 30, 40, 240), BLACK);
		text_draw(toast_msg, 160, 189, 0.45f, WHITE, 300, 1);
	}
	if (ringing) draw_ring_overlay();
	memcpy(pbtns, btns, nbtns * sizeof(Btn));
	npbtns = nbtns;
}

// ------------------------------------------------------------ input

static void press_button(int id)
{
	switch (id) {
	case BTN_FAV: toggle_fav_current(); break;
	case BTN_PLAYSTOP: play_or_stop(); break;
	case BTN_RANDOM: play_random(); break;
	case BTN_VOLDN: g_cfg.volume = g_cfg.volume >= 5 ? g_cfg.volume - 5 : 0; break;
	case BTN_VOLUP: g_cfg.volume = g_cfg.volume <= 95 ? g_cfg.volume + 5 : 100; break;
	case BTN_ALARM_TOGGLE:
		g_cfg.alarm_on = !g_cfg.alarm_on;
		if (g_cfg.alarm_on) {
			struct tm lt;
			gmtime_r(&now_t, &lt);
			// don't fire immediately if we're inside the alarm minute right now
			last_fired_yday = (lt.tm_hour == g_cfg.alarm_h && lt.tm_min == g_cfg.alarm_m) ? lt.tm_yday : -1;
		}
		config_save();
		break;
	case BTN_AH_DN: g_cfg.alarm_h = (g_cfg.alarm_h + 23) % 24; last_fired_yday = -1; break;
	case BTN_AH_UP: g_cfg.alarm_h = (g_cfg.alarm_h + 1) % 24; last_fired_yday = -1; break;
	case BTN_AM_DN: g_cfg.alarm_m = (g_cfg.alarm_m + 55) % 60 / 5 * 5; last_fired_yday = -1; break;
	case BTN_AM_UP: g_cfg.alarm_m = (g_cfg.alarm_m / 5 * 5 + 5) % 60; last_fired_yday = -1; break;
	case BTN_ASRC_PREV: g_cfg.alarm_src = g_cfg.alarm_src <= -1 ? g_nfavs - 1 : g_cfg.alarm_src - 1; break;
	case BTN_ASRC_NEXT: g_cfg.alarm_src = g_cfg.alarm_src >= g_nfavs - 1 ? -1 : g_cfg.alarm_src + 1; break;
	case BTN_SLEEP_PREV:
	case BTN_SLEEP_NEXT: {
		int n = sizeof sleep_opts / sizeof sleep_opts[0];
		sleep_idx = (sleep_idx + (id == BTN_SLEEP_NEXT ? 1 : n - 1)) % n;
		sleep_end = sleep_opts[sleep_idx] ? now_ms + sleep_opts[sleep_idx] * 60000ULL : 0;
		break;
	}
	case BTN_CLOCKMODE: mode = MODE_CLOCK; break;
	case BTN_DARKMODE: mode = MODE_DARK; break;
	case BTN_TEST_ALARM: start_alarm(); break;
	case BTN_SPK_LID: g_cfg.spk_lid = !g_cfg.spk_lid; config_save(); break;
	case BTN_UPDATE:
		if (updater_state() == UPD_AVAILABLE) updater_install();
		else if (!updater_busy() && updater_state() != UPD_DONE) updater_check();
		break;
	case BTN_TZ_DN: if (g_cfg.tz > -720) g_cfg.tz -= 30; break;
	case BTN_TZ_UP: if (g_cfg.tz < 840) g_cfg.tz += 30; break;
	case BTN_SEARCH_FIELD: do_search(); break;
	case BTN_RING_CONT: ring_continue(); break;
	case BTN_RING_OFF: ring_off(); break;
	case BTN_RING_SNOOZE: ring_snooze(); break;
	default:
		if (id >= BTN_TAB0 && id < BTN_TAB0 + NTABS) tab = id - BTN_TAB0;
		break;
	}
	if (id >= BTN_ALARM_TOGGLE && id <= BTN_ASRC_NEXT) config_save();
	if (id == BTN_TZ_DN || id == BTN_TZ_UP || id == BTN_VOLDN || id == BTN_VOLUP) config_save();
}

static bool repeatable(int id)
{
	return id == BTN_VOLDN || id == BTN_VOLUP || (id >= BTN_AH_DN && id <= BTN_AM_UP) || id == BTN_TZ_DN || id == BTN_TZ_UP;
}

static void handle_touch(u32 down, u32 held, u32 up)
{
	touchPosition tp;
	hidTouchRead(&tp);
	if (down & KEY_TOUCH) {
		held_btn = -1;
		for (int i = npbtns - 1; i >= 0; i--) {
			Btn *b = &pbtns[i];
			if (tp.px >= b->x && tp.px < b->x + b->w && tp.py >= b->y && tp.py < b->y + b->h) {
				held_btn = b->id;
				held_frames = 0;
				press_button(b->id);
				return;
			}
		}
		// list area?
		touch_in_list = false;
		dragging = false;
		if (!ringing && lst.tab == tab && list_rows(tab) && tp.py >= lst.y0 && tp.py < lst.y0 + lst.rows * ROW_H && tp.px < 316) {
			touch_in_list = true;
			touch_start_y = tp.py;
			touch_start_top = top_row[tab];
		}
	} else if ((held & KEY_TOUCH) && held_btn >= 0) {
		if (repeatable(held_btn) && ++held_frames > 20 && held_frames % 4 == 0) press_button(held_btn);
	} else if ((held & KEY_TOUCH) && touch_in_list) {
		int dy = tp.py - touch_start_y;
		if (abs(dy) > 6) dragging = true;
		if (dragging) {
			int n = list_count(tab), rows = list_rows(tab);
			int t = touch_start_top - dy / ROW_H;
			if (t > n - rows) t = n - rows;
			if (t < 0) t = 0;
			top_row[tab] = t;
		}
	}
	if (up & KEY_TOUCH) {
		if (touch_in_list && !dragging) {
			int i = top_row[tab] + (int)((touch_start_y - lst.y0) / ROW_H);
			if (i < list_count(tab)) {
				sel[tab] = i;
				list_active = true;
				activate_row(tab, i);
			}
		}
		touch_in_list = dragging = false;
		held_btn = -1;
	}
}

static void handle_keys(u32 down, u32 rep, u32 held)
{
	if (ringing) {
		if (down & KEY_A) ring_continue();
		else if (down & KEY_B) ring_off();
		else if (down & KEY_X) ring_snooze();
		return;
	}
	if (down & KEY_SELECT) {
		mode = (mode + 1) % 3;
		if (mode == MODE_NORMAL) toast("Telas ligadas");
	}
	if (mode != MODE_NORMAL) {
		// only volume works with screens off/dimmed
		if (rep & KEY_DUP) press_button(BTN_VOLUP);
		if (rep & KEY_DDOWN) press_button(BTN_VOLDN);
		return;
	}

	if (rep & KEY_DLEFT) tab = (tab + NTABS - 1) % NTABS;
	if (rep & KEY_DRIGHT) tab = (tab + 1) % NTABS;

	int rows = list_rows(tab);
	if (rows) {
		if (rep & (KEY_DUP | KEY_DDOWN)) {
			if (list_active) sel[tab] += (rep & KEY_DUP) ? -1 : 1;
			list_active = true;
			ensure_visible(tab);
		}
	} else if (tab == TAB_NOW) {
		if (rep & KEY_DUP) press_button(BTN_VOLUP);
		if (rep & KEY_DDOWN) press_button(BTN_VOLDN);
	}

	if (down & KEY_A) {
		if (tab == TAB_SEARCH && (!nhits || !list_active)) do_search();
		else if (rows && list_active && list_count(tab) > 0) activate_row(tab, sel[tab]);
		else play_focus();
	}
	if (down & KEY_B) player_stop();
	if (down & KEY_X) {
		if (rows && list_active && list_count(tab) > 0) fav_selected();
		else toggle_fav_current();
	}
	if (down & KEY_Y) {
		if (tab == TAB_SEARCH) do_search();
		else play_random();
	}
}

static void update_globe(u32 held)
{
	circlePosition cp, cs;
	hidCircleRead(&cp);
	float dx = abs(cp.dx) > 18 ? cp.dx / 156.0f : 0;
	float dy = abs(cp.dy) > 18 ? cp.dy / 156.0f : 0;
	if (mode != MODE_NORMAL || ringing) dx = dy = 0;
	if (dx || dy) {
		anim = false;
		idle_frames = 0;
		float sp = 1.8f / zoom;
		vlon += dx * fabsf(dx) * sp * 1.5f + dx * sp * 0.4f;
		vlat += dy * fabsf(dy) * sp * 1.5f + dy * sp * 0.4f;
		if (list_active && tab != TAB_HERE) list_active = false;
	} else idle_frames++;

	if (mode == MODE_NORMAL && !ringing) {
		if (held & KEY_R) zoom *= 1.03f;
		if (held & KEY_L) zoom /= 1.03f;
		hidCstickRead(&cs);
		if (abs(cs.dy) > 20) zoom *= 1 + cs.dy / 156.0f * 0.04f;
	}
	if (zoom < 1) zoom = 1;
	if (zoom > 12) zoom = 12;

	if (anim) {
		float dl = alat - vlat, dn = wrap180(alon - vlon);
		vlat += dl * 0.12f;
		vlon += dn * 0.12f;
		if (fabsf(dl) < 0.03f && fabsf(dn) < 0.03f) { vlat = alat; vlon = alon; anim = false; }
	}
	vlon = wrap180(vlon);
	if (vlat > 85) vlat = 85;
	if (vlat < -85) vlat = -85;

	// nearest city: only search again when the view moved
	static float last_lat = 999, last_lon = 999, dot;
	static int f = -1;
	if (vlat != last_lat || vlon != last_lon) {
		f = db_nearest_place(vlat, vlon, &dot);
		last_lat = vlat;
		last_lon = vlon;
	}
	if (f != focus) {
		focus = f;
		sel[TAB_HERE] = top_row[TAB_HERE] = 0;
	}
	// gentle snap onto the nearest city once the stick is released
	if (!anim && idle_frames > 8 && focus >= 0) {
		float ang = acosf(dot > 1 ? 1 : dot) * 180.0f / (float)M_PI;
		if (ang > 0.01f && ang < 8.0f / zoom) {
			const Place *p = &g_places[focus];
			vlat += (p->lat - vlat) * 0.15f;
			vlon += wrap180(p->lon - vlon) * 0.15f;
		}
	}
}

// ------------------------------------------------------------ main

static void fatal_screen(const char *l1, const char *l2)
{
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) break;
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		text_frame();
		C2D_TargetClear(t_bot, WHITE);
		C2D_SceneBegin(t_bot);
		text_draw(l1, 160, 90, 0.5f, BLACK, 300, 1);
		text_draw(l2, 160, 110, 0.45f, GRAY, 300, 1);
		text_draw("START: sair", 160, 150, 0.45f, GRAY, 300, 1);
		C2D_TargetClear(t_top, BLACK);
		C3D_FrameEnd(0);
	}
}

int main(int argc, char **argv)
{
	updater_init("ChunoB/Radio3DS", APP_VERSION, "Radio3DS.3dsx", argc > 0 ? argv[0] : NULL);
	romfsInit();
	gfxInitDefault();
	gspLcdInit();
	ptmuInit();
	osSetSpeedupEnable(true);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(8192);
	C2D_Prepare();
	t_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	t_top_r = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
	t_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	text_init();

	u32 *socbuf = memalign(0x1000, 0x100000);
	bool net_ok = socbuf && R_SUCCEEDED(socInit(socbuf, 0x100000));
	curl_global_init(CURL_GLOBAL_DEFAULT);

	aptHookCookie cookie;
	aptHook(&cookie, apt_cb, NULL);
	aptSetHomeAllowed(false);   // see go_home()
	hidSetRepeatParameters(18, 5);

	if (!db_load()) {
		fatal_screen("Não achei a lista de rádios.", "O arquivo romfs está faltando.");
		goto out;
	}
	config_load();
	globe_init();
	audio_ok = player_init();
	speaker_init(audio_ok);
	vlat = g_cfg.view_lat;
	vlon = g_cfg.view_lon;
	zoom = g_cfg.zoom;
	if (!net_ok) toast("Rede indisponível");

	// frame profiler: create sdmc:/3ds/Radio3DS/perf to see timings on screen and in perf.log
	FILE *pf = fopen(APP_DIR "/perf", "r");
	bool prof = pf != NULL;
	if (pf) fclose(pf);
	u64 pacc[6] = {0}, pt[7];
	float pcpu = 0, pgpu = 0;
	int pn = 0;
	char pmsg[160] = "";
#define PT(i) do { if (prof) pt[i] = svcGetSystemTick(); } while (0)

	while (aptMainLoop()) {
		if (aptCheckHomePressRejected()) {
			go_home();
			if (aptShouldClose()) break;
		}
		PT(0);
		// look for a new version once, a few seconds after starting
		static u64 t_start;
		static bool upd_checked, upd_told;
		if (!t_start) t_start = osGetTime();
		if (net_ok && !upd_checked && osGetTime() - t_start > 4000) { upd_checked = true; updater_check(); }
		if (!upd_told && updater_state() == UPD_AVAILABLE) {
			upd_told = true;
			char m[96];
			snprintf(m, sizeof m, "Versão %s disponível: Ajustes > Instalar", updater_latest());
			toast(m);
		}
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp(), rep = hidKeysDownRepeat();
		now_ms = osGetTime();
		now_t = time(NULL);

		if ((down & KEY_START) && mode == MODE_NORMAL && !ringing) break;

		handle_keys(down, rep, held);
		if (mode == MODE_NORMAL || ringing) handle_touch(down, held, up);
		update_globe(held);
		update_alarm();
		update_power();
		PT(1);

		if (!bl_top && !bl_bot) {
			gspWaitForVBlank();   // screens off: skip drawing, save battery
			continue;
		}

		float slider = osGet3DSliderState();
		bool stereo = bl_top && slider > 0.01f;
		if (stereo != stereo_on) { gfxSet3D(stereo); stereo_on = stereo; }

		if (bl_bot && tab == TAB_NOW) player_spectrum(bands, 32);
		PT(2);

		GlobeView gv = {
			.lat = vlat, .lon = vlon, .zoom = zoom, .focus = focus,
			.playing_place = player_state() != PL_STOPPED ? now_place : -1,
			.playing = player_state() == PL_PLAYING,
			.buffering = player_state() == PL_CONNECTING || player_state() == PL_BUFFERING,
			.error = player_state() == PL_ERROR,
			.now_local = now_t, .user_tz = g_cfg.tz,
			.alarm_on = g_cfg.alarm_on, .alarm_h = g_cfg.alarm_h, .alarm_m = g_cfg.alarm_m,
		};

		char c1[128] = "", c2[256] = "";
		if (mode != MODE_NORMAL) {
			if (ringing) snprintf(c1, sizeof c1, "ALARME!  A: continuar  B: desligar  X: soneca");
			else if (g_cfg.alarm_on) snprintf(c1, sizeof c1, "Alarme %02d:%02d", g_cfg.alarm_h, g_cfg.alarm_m);
			else if (snooze_until) snprintf(c1, sizeof c1, "Soneca ativa");
			if (player_state() != PL_STOPPED) {
				char t[200];
				player_title(t, sizeof t);
				snprintf(c2, sizeof c2, t[0] ? "%s · %s" : "%s", now_name, t);
			} else if (sleep_end) {
				snprintf(c2, sizeof c2, "Desliga em %d min", (int)((sleep_end - now_ms) / 60000) + 1);
			}
		}

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		text_frame();
		if (bl_top) {
			bool clock = mode != MODE_NORMAL;
			if (clock) clock_draw(t_top, &gv, stereo ? -1 : 0, slider, c1, c2);
			else globe_draw(t_top, &gv, stereo ? -1 : 0, slider);
			if (stereo) {
				if (clock) clock_draw(t_top_r, &gv, 1, slider, c1, c2);
				else globe_draw(t_top_r, &gv, 1, slider);
			}
			if (prof) text_draw(pmsg, 4, 226, 0.38f, C2D_Color32(0, 255, 0, 255), 0, 0);
		}
		PT(3);
		if (bl_bot) draw_bottom();
		PT(4);
		C3D_FrameEnd(0);
		PT(5);
		if (prof) {
			for (int i = 0; i < 5; i++) pacc[i] += pt[i + 1] - pt[i];
			pacc[5] += pt[5] - pt[0];
			pcpu += C3D_GetProcessingTime();
			pgpu += C3D_GetDrawingTime();
			if (++pn == 120) {
				double k = 1000.0 / SYSCLOCK_ARM11 / pn;
				snprintf(pmsg, sizeof pmsg, "upd %.1f fft %.1f top %.1f bot %.1f end %.1f | tot %.1f ms | c3d cpu %.1f gpu %.1f",
				         pacc[0] * k, pacc[1] * k, pacc[2] * k, pacc[3] * k, pacc[4] * k, pacc[5] * k, pcpu / pn, pgpu / pn);
				FILE *lf = fopen(APP_DIR "/perf.log", "a");
				if (lf) { fprintf(lf, "tab %d mode %d play %d: %s\n", tab, mode, player_state(), pmsg); fclose(lf); }
				memset(pacc, 0, sizeof pacc);
				pcpu = pgpu = 0;
				pn = 0;
			}
		}
	}

	g_cfg.view_lat = vlat;
	g_cfg.view_lon = vlon;
	g_cfg.zoom = zoom;
	config_save();

out:
	updater_exit();
	speaker_exit();
	player_exit();
	aptSetSleepAllowed(true);
	aptUnhook(&cookie);
	GSPLCD_PowerOnAllBacklights();
	curl_global_cleanup();
	if (net_ok) socExit();
	text_exit();
	C2D_Fini();
	C3D_Fini();
	ptmuExit();
	gspLcdExit();
	gfxExit();
	romfsExit();
	return 0;
}
