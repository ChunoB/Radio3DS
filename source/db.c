#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

Place   *g_places;   int g_nplaces;
Station *g_stations; int g_nstations;

static char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(n + 1);
	if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
	fclose(f);
	if (buf) { buf[n] = 0; if (len) *len = n; }
	return buf;
}

static int count_lines(const char *s)
{
	int n = 0;
	for (; *s; s++) if (*s == '\n') n++;
	return n;
}

// Splits a line in place on tabs. Returns number of fields; advances *p past the line.
static int split_line(char **p, char **fields, int max)
{
	char *s = *p;
	if (!*s) return 0;
	int n = 0;
	fields[n++] = s;
	for (; *s && *s != '\n'; s++) {
		if (*s == '\t') {
			*s = 0;
			if (n < max) fields[n++] = s + 1;
		}
	}
	if (*s == '\n') *s++ = 0;
	*p = s;
	return n;
}

bool db_load(void)
{
	// a newer list made by the PC tool (pc/atualizar_radios.bat) wins over the built-in one
	char *cities = slurp(APP_DIR "/cities.tsv", NULL);
	char *stations = cities ? slurp(APP_DIR "/stations.tsv", NULL) : NULL;
	if (!cities || !stations) {
		free(cities);
		free(stations);
		cities = slurp("romfs:/cities.tsv", NULL);
		stations = slurp("romfs:/stations.tsv", NULL);
	}
	if (!cities || !stations) return false;

	int nc = count_lines(cities), ns = count_lines(stations);
	g_places = calloc(nc, sizeof(Place));
	g_stations = calloc(ns, sizeof(Station));
	if (!g_places || !g_stations) return false;

	char *p = cities, *f[8];
	const float d2r = (float)M_PI / 180.0f;
	while (g_nplaces < nc) {
		int n = split_line(&p, f, 8);
		if (n == 0) break;
		if (n < 8) continue;
		Place *pl = &g_places[g_nplaces++];
		pl->name = f[0];
		pl->country = f[1];
		pl->lat = atof(f[2]);
		pl->lon = atof(f[3]);
		pl->tz_std = atoi(f[4]);
		pl->tz_dst = atoi(f[5]);
		pl->first = atoi(f[6]);
		pl->count = atoi(f[7]);
		pl->slat = sinf(pl->lat * d2r); pl->clat = cosf(pl->lat * d2r);
		pl->slon = sinf(pl->lon * d2r); pl->clon = cosf(pl->lon * d2r);
	}

	p = stations;
	while (g_nstations < ns) {
		int n = split_line(&p, f, 4);
		if (n == 0) break;
		Station *st = &g_stations[g_nstations++];
		st->name = f[0];
		st->url = n > 1 ? f[1] : "";
		st->bitrate = n > 2 ? atoi(f[2]) : 0;
		st->tags = n > 3 ? f[3] : "";
		st->place = -1;
	}
	for (int i = 0; i < g_nplaces; i++) {
		Place *pl = &g_places[i];
		if (pl->first < 0 || pl->first + pl->count > g_nstations) { pl->count = 0; continue; }
		for (int j = 0; j < pl->count; j++) g_stations[pl->first + j].place = i;
	}
	return g_nplaces > 0 && g_nstations > 0;
}

int db_nearest_place(float lat, float lon, float *out_dot)
{
	const float d2r = (float)M_PI / 180.0f;
	float sl = sinf(lat * d2r), cl = cosf(lat * d2r);
	float so = sinf(lon * d2r), co = cosf(lon * d2r);
	int best = -1;
	float bd = -2;
	for (int i = 0; i < g_nplaces; i++) {
		const Place *p = &g_places[i];
		// cos of angular distance
		float d = sl * p->slat + cl * p->clat * (co * p->clon + so * p->slon);
		if (d > bd) { bd = d; best = i; }
	}
	if (out_dot) *out_dot = bd;
	return best;
}

int place_utc_offset(const Place *p, time_t utc)
{
	if (p->tz_std == p->tz_dst) return p->tz_std;
	struct tm tmv;
	gmtime_r(&utc, &tmv);
	int yd = tmv.tm_yday;   // 0-based
	bool dst;
	if (p->lat >= 0) dst = yd >= 85 && yd < 300;   // ~late March .. late October
	else             dst = yd >= 278 || yd < 95;   // ~early October .. early April
	return dst ? p->tz_dst : p->tz_std;
}
