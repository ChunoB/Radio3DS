#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Parsing text and measuring glyphs is slow on the Old 3DS, and most strings are the
// same from one frame to the next. So parsed text and widths are kept in a cache
// (keyed by string + scale + max width) and only rebuilt when the cache fills up.
#define TB_GLYPHS 12288
#define TC_SLOTS  512          // power of two
#define TC_KEY    192

typedef struct {
	bool  used, parsed;
	float scale, maxw;
	float width;               // width of the full string at this scale
	C2D_Text text;             // parsed (and truncated, if needed) text
	char  key[TC_KEY];
} TCEntry;

static C2D_TextBuf g_tb;
static TCEntry *g_tc;
static int g_tc_count;
static size_t g_tb_used;

static void tc_flush(void)
{
	C2D_TextBufClear(g_tb);
	g_tb_used = 0;
	memset(g_tc, 0, TC_SLOTS * sizeof(TCEntry));
	g_tc_count = 0;
}

void text_init(void)
{
	fontEnsureMapped();
	g_tb = C2D_TextBufNew(TB_GLYPHS);
	g_tc = calloc(TC_SLOTS, sizeof(TCEntry));
}

void text_exit(void)
{
	C2D_TextBufDelete(g_tb);
	free(g_tc);
}

void text_frame(void) {}

static int next_cp(const char *s, u32 *cp)
{
	ssize_t n = decode_utf8(cp, (const uint8_t *)s);
	if (n <= 0) { *cp = '?'; return 1; }
	return (int)n;
}

static float cp_width(u32 cp)
{
	int gi = fontGlyphIndexFromCodePoint(NULL, cp);
	charWidthInfo_s *w = fontGetCharWidthInfo(NULL, gi);
	return w ? w->charWidth : 0;
}

static float measure(const char *s, float scale)
{
	float w = 0;
	while (*s) {
		u32 cp;
		s += next_cp(s, &cp);
		if (cp == '\n') break;
		w += cp_width(cp);
	}
	return w * scale;
}

// Finds (or makes room for) the cache entry of s; NULL if s is too long to cache.
static TCEntry *tc_get(const char *s, float scale, float maxw)
{
	size_t len = strlen(s);
	if (!g_tc || len >= TC_KEY) return NULL;
	u32 h = 2166136261u;
	for (size_t i = 0; i < len; i++) h = (h ^ (u8)s[i]) * 16777619u;
	h ^= (u32)(scale * 1000) * 2654435761u;
	h ^= (u32)(maxw * 10) * 40503u;
	for (int n = 0; n < TC_SLOTS; n++) {
		TCEntry *e = &g_tc[(h + n) & (TC_SLOTS - 1)];
		if (!e->used) {
			if (g_tc_count >= TC_SLOTS * 3 / 4) { tc_flush(); return tc_get(s, scale, maxw); }
			e->used = true;
			e->parsed = false;
			e->scale = scale;
			e->maxw = maxw;
			memcpy(e->key, s, len + 1);
			e->width = measure(s, scale);
			g_tc_count++;
			return e;
		}
		if (e->scale == scale && e->maxw == maxw && !strcmp(e->key, s)) return e;
	}
	return NULL;
}

float text_width(const char *s, float scale)
{
	TCEntry *e = tc_get(s, scale, 0);
	return e ? e->width : measure(s, scale);
}

// Copies the longest prefix of s that fits maxw (in pixels); returns its byte length.
static size_t fit_prefix(const char *s, float scale, float maxw)
{
	float w = 0;
	const char *p = s;
	while (*p) {
		u32 cp;
		int n = next_cp(p, &cp);
		if (cp == '\n') break;
		float cw = cp_width(cp) * scale;
		if (w + cw > maxw) break;
		w += cw;
		p += n;
	}
	return p - s;
}

// Builds the string to show: s itself, or s cut to fit maxw with "..." at the end.
static const char *fitted(const char *s, float scale, float maxw, float width, char *tmp, size_t cap)
{
	if (maxw <= 0 || width <= maxw) return s;
	float dots = measure("...", scale);
	size_t n = fit_prefix(s, scale, maxw - dots);
	if (n > cap - 4) n = cap - 4;
	memcpy(tmp, s, n);
	while (n && tmp[n - 1] == ' ') n--;
	memcpy(tmp + n, "...", 4);
	return tmp;
}

static bool parse(C2D_Text *t, const char *str)
{
	size_t n = strlen(str);
	if (g_tb_used + n + 1 > TB_GLYPHS) tc_flush();
	if (n + 1 > TB_GLYPHS) return false;
	C2D_TextParse(t, g_tb, str);
	C2D_TextOptimize(t);
	g_tb_used += n + 1;
	return true;
}

void text_draw(const char *s, float x, float y, float scale, u32 color, float maxw, int align)
{
	if (!s || !*s) return;
	char tmp[600];
	C2D_Text t, nt, *tp = &t;
	TCEntry *e = tc_get(s, scale, maxw);
	if (e) {
		if (!e->parsed) {
			const char *str = fitted(s, scale, maxw, e->width, tmp, sizeof tmp);
			// parse() may flush the cache, which would wipe this entry: re-fetch it after
			size_t before = g_tc_count;
			if (!parse(&nt, str)) return;
			if ((size_t)g_tc_count < before) {
				e = tc_get(s, scale, maxw);
				if (!e) { tp = &nt; goto draw; }
			}
			e->text = nt;
			e->parsed = true;
		}
		tp = &e->text;
	} else {
		const char *str = fitted(s, scale, maxw, maxw > 0 ? measure(s, scale) : 0, tmp, sizeof tmp);
		if (!parse(&t, str)) return;
	}
draw:;
	u32 flags = C2D_WithColor;
	if (align == 1) flags |= C2D_AlignCenter;
	else if (align == 2) flags |= C2D_AlignRight;
	C2D_DrawText(tp, flags, x, y, 0.5f, scale, scale, color);
}

int text_wrap2(const char *s, float scale, float maxw, char *l1, char *l2, size_t n)
{
	l1[0] = l2[0] = 0;
	if (!s || !*s) return 0;
	size_t len = strlen(s);
	size_t cut = fit_prefix(s, scale, maxw);
	if (cut >= len) {
		snprintf(l1, n, "%s", s);
		return 1;
	}
	// back up to the last space, if there is one reasonably close
	size_t sp = cut;
	while (sp > 0 && s[sp] != ' ') sp--;
	if (sp > cut / 2) cut = sp;
	if (cut >= n) cut = n - 1;
	memcpy(l1, s, cut);
	l1[cut] = 0;
	const char *rest = s + cut;
	while (*rest == ' ') rest++;
	snprintf(l2, n, "%s", rest);
	return 2;
}

void utf8_sanitize(char *s)
{
	size_t len = strlen(s);
	char *out = malloc(len * 2 + 1);
	if (!out) return;
	size_t o = 0;
	const u8 *p = (const u8 *)s;
	while (*p) {
		u32 cp;
		ssize_t n = decode_utf8(&cp, p);
		if (n > 0 && (*p < 0x80 || n > 1)) {
			memcpy(out + o, p, n);
			o += n;
			p += n;
			continue;
		}
		u8 b = *p++;
		// Windows-1252 punctuation that shows up in station metadata
		if (b == 0x91 || b == 0x92) { out[o++] = '\''; continue; }
		if (b == 0x93 || b == 0x94) { out[o++] = '"'; continue; }
		if (b == 0x96 || b == 0x97) { out[o++] = '-'; continue; }
		if (b < 0xA0) { out[o++] = '?'; continue; }
		out[o++] = 0xC0 | (b >> 6);
		out[o++] = 0x80 | (b & 0x3F);
	}
	out[o] = 0;
	memcpy(s, out, o + 1);
	free(out);
}

void fold_text(const char *in, char *out, size_t n)
{
	static const char latin[] =
		"aaaaaaaceeeeiiiidnooooo*ouuuuyts"   // 0xC0..0xDF
		"aaaaaaaceeeeiiiidnooooo/ouuuuyty";  // 0xE0..0xFF
	size_t o = 0;
	const char *p = in;
	while (*p && o + 4 < n) {
		u32 cp;
		int k = next_cp(p, &cp);
		if (cp < 0x80) out[o++] = (char)tolower((int)cp);
		else if (cp >= 0xC0 && cp <= 0xFF) out[o++] = latin[cp - 0xC0];
		else { memcpy(out + o, p, k); o += k; }
		p += k;
	}
	out[o] = 0;
}
