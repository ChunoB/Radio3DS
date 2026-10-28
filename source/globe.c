// Top screen: orthographic globe with station dots, day/night shading,
// drifting stars, the odd comet, and a big amber bedside clock.
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TOP_W 400
#define TOP_H 240
#define NSTARS 170
#define D2R ((float)M_PI / 180.0f)

typedef struct { float x, y, b, f, ph; u8 s; } Star;
static Star stars[NSTARS];

static struct { bool on; float x, y, vx, vy; int life; } comet;
static int comet_wait = 900;
static u32 frame;

void globe_init(void)
{
	srand(1234);
	for (int i = 0; i < NSTARS; i++) {
		stars[i].x = (float)(rand() % 800);
		stars[i].y = (float)(rand() % 480);
		stars[i].b = 0.35f + (rand() % 65) / 100.0f;
		stars[i].f = 0.02f + (rand() % 60) / 1000.0f;
		stars[i].ph = (rand() % 628) / 100.0f;
		stars[i].s = (rand() % 9) == 0 ? 2 : 1;
	}
	srand((unsigned)osGetTime());
}

static inline u32 rgba(int r, int g, int b, int a)
{
	return C2D_Color32(r < 0 ? 0 : r > 255 ? 255 : r, g < 0 ? 0 : g > 255 ? 255 : g,
	                   b < 0 ? 0 : b > 255 ? 255 : b, a < 0 ? 0 : a > 255 ? 255 : a);
}

static void ring(float cx, float cy, float r, float th, u32 c)
{
	const int seg = 24;
	for (int i = 0; i < seg; i++) {
		float a0 = i * 2 * (float)M_PI / seg, a1 = (i + 1) * 2 * (float)M_PI / seg;
		C2D_DrawLine(cx + cosf(a0) * r, cy + sinf(a0) * r, c, cx + cosf(a1) * r, cy + sinf(a1) * r, c, th, 0.5f);
	}
}

// view-space vector of a lat/lon given the view rotation
typedef struct { float sl0, cl0, so0, co0; } Rot;

static inline void project(const Rot *r, float slat, float clat, float slon, float clon,
                           float *x, float *y, float *z)
{
	float cd = clon * r->co0 + slon * r->so0;   // cos(lon - lon0)
	float sd = slon * r->co0 - clon * r->so0;   // sin(lon - lon0)
	*x = clat * sd;
	*y = r->cl0 * slat - r->sl0 * clat * cd;
	*z = r->sl0 * slat + r->cl0 * clat * cd;
}

static void sun_vector(const Rot *r, time_t utc, float *sx, float *sy, float *sz)
{
	struct tm tmv;
	gmtime_r(&utc, &tmv);
	float hours = tmv.tm_hour + tmv.tm_min / 60.0f + tmv.tm_sec / 3600.0f;
	float decl = -23.44f * cosf(2 * (float)M_PI * (tmv.tm_yday + 10) / 365.0f);
	float lon = (12.0f - hours) * 15.0f;
	project(r, sinf(decl * D2R), cosf(decl * D2R), sinf(lon * D2R), cosf(lon * D2R), sx, sy, sz);
}

// Shades the night half of the visible disc with horizontal spans.
static void draw_night(float cx, float cy, float R, float sx, float sy, float sz, float ex)
{
	const u32 shade = C2D_Color32(0, 0, 18, 120);
	float A = sqrtf(sx * sx + sz * sz);
	float phi = atan2f(sz, sx);
	int y0 = (int)(cy - R), y1 = (int)(cy + R);
	if (y0 < 0) y0 = 0;
	if (y1 > TOP_H) y1 = TOP_H;
	for (int py = y0; py < y1; py++) {
		float yn = (cy - (py + 0.5f)) / R;
		float r2 = 1 - yn * yn;
		if (r2 <= 0) continue;
		float rr = sqrtf(r2);
		// candidate span boundaries in theta (x = rr*cos(theta), z = rr*sin(theta), theta in [0, pi])
		float th[6];
		int nt = 0;
		th[nt++] = 0;
		if (A > 1e-4f) {
			float k = -yn * sy / (rr * A);
			if (k > -1 && k < 1) {
				float a = acosf(k);
				float c[2] = { phi - a, phi + a };
				for (int i = 0; i < 2; i++) {
					float t = fmodf(c[i], 2 * (float)M_PI);
					if (t < 0) t += 2 * (float)M_PI;
					if (t > 0 && t < (float)M_PI) th[nt++] = t;
				}
			}
		}
		th[nt++] = (float)M_PI;
		if (nt == 4 && th[1] > th[2]) { float t = th[1]; th[1] = th[2]; th[2] = t; }
		for (int i = 0; i + 1 < nt; i++) {
			float m = 0.5f * (th[i] + th[i + 1]);
			float f = rr * (sx * cosf(m) + sz * sinf(m)) + yn * sy;
			if (f >= 0) continue;
			float xa = cx + rr * cosf(th[i + 1]) * R + ex;
			float xb = cx + rr * cosf(th[i]) * R + ex;
			if (xb < 0 || xa > TOP_W) continue;
			if (xa < 0) xa = 0;
			if (xb > TOP_W) xb = TOP_W;
			C2D_DrawRectSolid(xa, py, 0.5f, xb - xa, 1, shade);
		}
	}
}

static void draw_stars(const GlobeView *v, float eye, float slider)
{
	float t = frame;
	float shift = eye * slider * 6.0f;   // far behind the screen
	for (int i = 0; i < NSTARS; i++) {
		Star *s = &stars[i];
		float x = fmodf(s->x - v->lon * 1.4f + t * 0.03f, 800.0f);
		if (x < 0) x += 800;
		float y = fmodf(s->y + v->lat * 1.4f + 480.0f * 4, 480.0f);
		x -= 200; y -= 120;
		if (x < -2 || x > TOP_W || y < -2 || y > TOP_H) continue;
		float b = s->b * (0.7f + 0.3f * sinf(t * s->f + s->ph));
		int c = (int)(255 * b);
		C2D_DrawRectSolid(x + shift, y, 0.5f, s->s, s->s, rgba(c, c, (int)(c * 1.05f + 10), 255));
	}

	if (comet.on) {
		float tx = comet.x - comet.vx * 14, ty = comet.y - comet.vy * 14;
		int a = comet.life > 30 ? 255 : comet.life * 8;
		C2D_DrawLine(comet.x + shift, comet.y, rgba(255, 255, 255, a), tx + shift, ty, rgba(140, 190, 255, 0), 2.0f, 0.5f);
		C2D_DrawRectSolid(comet.x - 1 + shift, comet.y - 1, 0.5f, 2.5f, 2.5f, rgba(255, 255, 255, a));
	}
}

static void step_effects(float eye)
{
	if (eye > 0) return;   // advance once per frame (left eye / mono pass)
	frame++;
	if (comet.on) {
		comet.x += comet.vx;
		comet.y += comet.vy;
		if (--comet.life <= 0 || comet.x < -40 || comet.x > TOP_W + 40 || comet.y > TOP_H + 40) comet.on = false;
	} else if (--comet_wait <= 0) {
		comet.on = true;
		comet.x = 40 + rand() % 320;
		comet.y = -5;
		comet.vx = ((rand() % 2) ? 1 : -1) * (2.0f + (rand() % 20) / 10.0f);
		comet.vy = 1.0f + (rand() % 15) / 10.0f;
		comet.life = 110;
		comet_wait = 1800 + rand() % 3600;   // every 30-90 s
	}
}

static void fmt_hm(char *out, size_t n, time_t t)
{
	struct tm tmv;
	gmtime_r(&t, &tmv);
	snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

void globe_draw(C3D_RenderTarget *target, const GlobeView *v, float eye, float slider)
{
	C2D_TargetClear(target, C2D_Color32(2, 3, 12, 255));
	C2D_SceneBegin(target);
	step_effects(eye);

	const float cx = TOP_W / 2.0f, cy = TOP_H / 2.0f;
	const float R = 98.0f * v->zoom;
	Rot r = { sinf(v->lat * D2R), cosf(v->lat * D2R), sinf(v->lon * D2R), cosf(v->lon * D2R) };
	time_t utc = v->now_local - v->user_tz * 60;

	draw_stars(v, eye, slider);

	// atmosphere + ocean
	float gx = cx;   // disc sits on the screen plane
	C2D_DrawCircleSolid(gx, cy, 0.5f, R + 9, C2D_Color32(40, 90, 255, 30));
	C2D_DrawCircleSolid(gx, cy, 0.5f, R + 5, C2D_Color32(60, 120, 255, 55));
	C2D_DrawCircleSolid(gx, cy, 0.5f, R + 2, C2D_Color32(90, 160, 255, 110));
	C2D_DrawCircle(gx, cy, 0.5f, R,
	               C2D_Color32(40, 90, 230, 255), C2D_Color32(20, 60, 200, 255),
	               C2D_Color32(10, 35, 150, 255), C2D_Color32(6, 20, 110, 255));

	float sx, sy, sz;
	sun_vector(&r, utc, &sx, &sy, &sz);
	draw_night(cx, cy, R, sx, sy, sz, 0);

	// station dots. Drawing ~1,700 visible dots one by one is the most expensive part of
	// the frame on an Old 3DS, so dots that land on a 2x2 cell already taken are skipped
	// (they would be hidden anyway). Big cities are drawn first so they win the cell.
	static u32 lut_day[147], lut_night[147];
	static bool lut_ok;
	if (!lut_ok) {
		for (int a = 0; a < 147; a++) {
			lut_day[a] = rgba(255, 236, 150, 110 + a);
			lut_night[a] = rgba(255, 190, 70, 110 + a);
		}
		lut_ok = true;
	}
	static u8 taken[(TOP_W / 2 + 4) * (TOP_H / 2 + 4) / 8 + 1];
	memset(taken, 0, sizeof taken);
	float dscale = v->zoom > 1 ? 1 + (v->zoom - 1) * 0.12f : 1;
	if (dscale > 2.2f) dscale = 2.2f;
	float pop = -eye * slider * 5.0f;   // dots on the near face come out of the screen
	float cd0 = r.cl0, sd0 = r.sl0;
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < g_nplaces; i++) {
			const Place *p = &g_places[i];
			bool big = p->count >= 5;
			if (big != (pass == 0)) continue;
			// z first: half of the globe is on the far side
			float cd = p->clon * r.co0 + p->slon * r.so0;
			float z = sd0 * p->slat + cd0 * p->clat * cd;
			if (z <= 0.02f) continue;
			float sd = p->slon * r.co0 - p->clon * r.so0;
			float x = p->clat * sd;
			float y = cd0 * p->slat - sd0 * p->clat * cd;
			float px = cx + x * R, py = cy - y * R;
			if (px < -3 || px > TOP_W + 3 || py < -3 || py > TOP_H + 3) continue;
			int gx = ((int)px + 4) >> 1, gy = ((int)py + 4) >> 1;
			int bit = gy * (TOP_W / 2 + 4) + gx;
			if (taken[bit >> 3] & (1 << (bit & 7))) continue;
			taken[bit >> 3] |= 1 << (bit & 7);
			float lit = x * sx + y * sy + z * sz;
			int a = (int)(145 * z);
			if (a > 146) a = 146;
			u32 col = lit > 0 ? lut_day[a] : lut_night[a];
			float s = (p->count >= 15 ? 3.2f : big ? 2.6f : 2.0f) * dscale;
			px += pop * z;
			C2D_DrawRectSolid(px - s / 2, py - s / 2, 0.5f, s, s, col);
		}
	}

	// playing place: pulsing red marker
	if (v->playing_place >= 0) {
		const Place *p = &g_places[v->playing_place];
		float x, y, z;
		project(&r, p->slat, p->clat, p->slon, p->clon, &x, &y, &z);
		if (z > 0) {
			float px = cx + x * R + pop * z, py = cy - y * R;
			float pr = 3 + 2.0f * (0.5f + 0.5f * sinf(frame * 0.12f));
			C2D_DrawCircleSolid(px, py, 0.5f, pr + 2, C2D_Color32(255, 60, 60, 70));
			C2D_DrawCircleSolid(px, py, 0.5f, pr * 0.6f, C2D_Color32(255, 70, 60, 255));
		}
	}

	// crosshair
	float cpop = -eye * slider * 4.0f;
	ring(cx + cpop, cy, 11, 1.6f, C2D_Color32(255, 255, 255, 200));
	C2D_DrawRectSolid(cx + cpop - 1, cy - 1, 0.5f, 2, 2, C2D_Color32(255, 255, 255, 230));

	// header
	text_draw("RADIO 3DS", 10 + cpop, 6, 0.5f, C2D_Color32(255, 255, 255, 230), 0, 0);
	char hm[16], buf[96];
	fmt_hm(hm, sizeof hm, v->now_local);
	if (v->alarm_on) snprintf(buf, sizeof buf, "%s   Alarme %02d:%02d", hm, v->alarm_h, v->alarm_m);
	else snprintf(buf, sizeof buf, "%s", hm);
	text_draw(buf, TOP_W - 10 + cpop, 6, 0.45f, C2D_Color32(220, 225, 255, 220), 0, 2);

	// focused city label
	if (v->focus >= 0) {
		const Place *p = &g_places[v->focus];
		char hm2[16], info[160];
		fmt_hm(hm2, sizeof hm2, utc + place_utc_offset(p, utc) * 60);
		snprintf(info, sizeof info, "%s · %d %s · %s · A: ouvir", p->country, p->count,
		         p->count == 1 ? "rádio" : "rádios", hm2);
		float w1 = text_width(p->name, 0.5f), w2 = text_width(info, 0.42f);
		float w = (w1 > w2 ? w1 : w2) + 16;
		if (w > 360) w = 360;
		float bx = cx - w / 2 + cpop, by = 186;
		C2D_DrawRectSolid(bx, by, 0.5f, w, 34, C2D_Color32(245, 245, 245, 235));
		C2D_DrawRectSolid(bx, by + 34, 0.5f, w, 1, C2D_Color32(120, 120, 140, 255));
		text_draw(p->name, cx + cpop, by + 2, 0.5f, C2D_Color32(10, 10, 20, 255), w - 10, 1);
		text_draw(info, cx + cpop, by + 18, 0.42f, C2D_Color32(80, 80, 95, 255), w - 10, 1);
	}

	// tuned indicator, bottom-right
	if (v->playing || v->buffering || v->error) {
		float bx = 352 + cpop, by = 190;
		C2D_DrawRectSolid(bx, by, 0.5f, 36, 26, C2D_Color32(245, 245, 245, 235));
		u32 c;
		if (v->error) c = C2D_Color32(130, 130, 130, 255);
		else if (v->buffering) c = (frame / 15) & 1 ? C2D_Color32(240, 190, 30, 255) : C2D_Color32(240, 190, 30, 90);
		else c = C2D_Color32(230, 30, 30, 255);
		C2D_DrawCircleSolid(bx + 18, by + 13, 0.5f, 6.5f, c);
	}
}

// ------------------------------------------------------------ bedside clock

static void seg_digit(float x, float y, float w, float h, float t, int d, u32 on, u32 off)
{
	static const u8 map[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
	u8 m = d >= 0 && d <= 9 ? map[d] : 0;
	float hh = (h - 3 * t) / 2;   // vertical segment length
	float g = 1.5f;               // gap
	// a b c d e f g
	float rx[7] = { x + t + g, x + w - t, x + w - t, x + t + g, x, x, x + t + g };
	float ry[7] = { y, y + t + g, y + 2 * t + hh + g, y + h - t, y + 2 * t + hh + g, y + t + g, y + t + hh };
	float rw[7] = { w - 2 * t - 2 * g, t, t, w - 2 * t - 2 * g, t, t, w - 2 * t - 2 * g };
	float rh[7] = { t, hh - 2 * g, hh - 2 * g, t, hh - 2 * g, hh - 2 * g, t };
	for (int i = 0; i < 7; i++)
		C2D_DrawRectSolid(rx[i], ry[i], 0.5f, rw[i], rh[i], (m >> i) & 1 ? on : off);
}

void clock_draw(C3D_RenderTarget *target, const GlobeView *v, float eye, float slider,
                const char *line1, const char *line2)
{
	C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
	C2D_SceneBegin(target);
	step_effects(eye);

	struct tm tmv;
	gmtime_r(&v->now_local, &tmv);
	const u32 on = C2D_Color32(255, 150, 20, 255), off = C2D_Color32(40, 20, 4, 255);
	float pop = -eye * slider * 7.0f;
	float w = 66, h = 120, t = 12, gap = 14, colon = 26;
	float total = 4 * w + 3 * gap + colon - gap;
	float x = (TOP_W - total) / 2 + pop, y = 34;
	seg_digit(x, y, w, h, t, tmv.tm_hour / 10, on, off); x += w + gap;
	seg_digit(x, y, w, h, t, tmv.tm_hour % 10, on, off); x += w;
	u32 cc = (tmv.tm_sec & 1) ? on : off;
	C2D_DrawRectSolid(x + colon / 2 - 5, y + h * 0.3f, 0.5f, 10, 10, cc);
	C2D_DrawRectSolid(x + colon / 2 - 5, y + h * 0.7f - 10, 0.5f, 10, 10, cc);
	x += colon;
	seg_digit(x, y, w, h, t, tmv.tm_min / 10, on, off); x += w + gap;
	seg_digit(x, y, w, h, t, tmv.tm_min % 10, on, off);

	static const char *dias[7] = { "domingo", "segunda", "terça", "quarta", "quinta", "sexta", "sábado" };
	static const char *meses[12] = { "jan", "fev", "mar", "abr", "mai", "jun", "jul", "ago", "set", "out", "nov", "dez" };
	char buf[96];
	snprintf(buf, sizeof buf, "%s, %d %s", dias[tmv.tm_wday % 7], tmv.tm_mday, meses[tmv.tm_mon % 12]);
	float tp = -eye * slider * 3.0f;
	const u32 dim = C2D_Color32(200, 120, 20, 255);
	text_draw(buf, 200 + tp, 168, 0.55f, dim, 380, 1);
	if (line1 && *line1) text_draw(line1, 200 + tp, 192, 0.5f, dim, 380, 1);
	if (line2 && *line2) text_draw(line2, 200 + tp, 212, 0.45f, C2D_Color32(150, 90, 15, 255), 380, 1);
}
