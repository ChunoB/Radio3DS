#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

Config g_cfg;
Fav    g_favs[MAX_FAVS];
int    g_nfavs;

static void defaults(void)
{
	memset(&g_cfg, 0, sizeof g_cfg);
	g_cfg.volume = 80;
	g_cfg.tz = -180;            // Brasília
	g_cfg.alarm_h = 7;
	g_cfg.alarm_m = 0;
	g_cfg.alarm_src = -1;
	g_cfg.view_lat = -15.8f;
	g_cfg.view_lon = -47.9f;
	g_cfg.zoom = 1.0f;
	g_cfg.last_place = -1;
	g_cfg.spk_lid = false;      // experimental: writing codec registers may hang HOME on real hardware
}

static void chomp(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

void config_load(void)
{
	defaults();
	mkdir("sdmc:/3ds", 0777);
	mkdir(APP_DIR, 0777);

	FILE *f = fopen(APP_DIR "/config.ini", "r");
	if (f) {
		char line[700];
		while (fgets(line, sizeof line, f)) {
			chomp(line);
			char *eq = strchr(line, '=');
			if (!eq) continue;
			*eq = 0;
			const char *k = line, *v = eq + 1;
			if (!strcmp(k, "volume")) g_cfg.volume = atoi(v);
			else if (!strcmp(k, "tz")) g_cfg.tz = atoi(v);
			else if (!strcmp(k, "alarm_on")) g_cfg.alarm_on = atoi(v) != 0;
			else if (!strcmp(k, "alarm_h")) g_cfg.alarm_h = atoi(v);
			else if (!strcmp(k, "alarm_m")) g_cfg.alarm_m = atoi(v);
			else if (!strcmp(k, "alarm_src")) g_cfg.alarm_src = atoi(v);
			else if (!strcmp(k, "view_lat")) g_cfg.view_lat = atof(v);
			else if (!strcmp(k, "view_lon")) g_cfg.view_lon = atof(v);
			else if (!strcmp(k, "zoom")) g_cfg.zoom = atof(v);
			else if (!strcmp(k, "last_name")) snprintf(g_cfg.last_name, sizeof g_cfg.last_name, "%s", v);
			else if (!strcmp(k, "last_url")) snprintf(g_cfg.last_url, sizeof g_cfg.last_url, "%s", v);
			else if (!strcmp(k, "last_place")) g_cfg.last_place = atoi(v);
			else if (!strcmp(k, "spk_lid2")) g_cfg.spk_lid = atoi(v) != 0;
		}
		fclose(f);
	}
	if (g_cfg.volume < 0 || g_cfg.volume > 100) g_cfg.volume = 80;
	if (g_cfg.alarm_h < 0 || g_cfg.alarm_h > 23) g_cfg.alarm_h = 7;
	if (g_cfg.alarm_m < 0 || g_cfg.alarm_m > 59) g_cfg.alarm_m = 0;
	if (g_cfg.zoom < 1 || g_cfg.zoom > 12) g_cfg.zoom = 1;
	if (g_cfg.last_place >= g_nplaces) g_cfg.last_place = -1;

	// favorites.tsv: name<TAB>url<TAB>place   (you can add your own stations here)
	g_nfavs = 0;
	f = fopen(APP_DIR "/favorites.tsv", "r");
	if (f) {
		char line[700];
		while (g_nfavs < MAX_FAVS && fgets(line, sizeof line, f)) {
			chomp(line);
			char *t1 = strchr(line, '\t');
			if (!t1) continue;
			*t1 = 0;
			char *url = t1 + 1;
			char *t2 = strchr(url, '\t');
			int place = -1;
			if (t2) { *t2 = 0; place = atoi(t2 + 1); }
			if (strncmp(url, "http", 4)) continue;
			Fav *fv = &g_favs[g_nfavs++];
			snprintf(fv->name, sizeof fv->name, "%s", line);
			snprintf(fv->url, sizeof fv->url, "%s", url);
			fv->place = (place >= 0 && place < g_nplaces) ? place : -1;
		}
		fclose(f);
	}
	if (g_cfg.alarm_src >= g_nfavs) g_cfg.alarm_src = -1;
}

void config_save(void)
{
	FILE *f = fopen(APP_DIR "/config.ini", "w");
	if (!f) return;
	fprintf(f, "volume=%d\ntz=%d\nalarm_on=%d\nalarm_h=%d\nalarm_m=%d\nalarm_src=%d\n",
	        g_cfg.volume, g_cfg.tz, g_cfg.alarm_on, g_cfg.alarm_h, g_cfg.alarm_m, g_cfg.alarm_src);
	fprintf(f, "view_lat=%.3f\nview_lon=%.3f\nzoom=%.2f\n", g_cfg.view_lat, g_cfg.view_lon, g_cfg.zoom);
	fprintf(f, "last_name=%s\nlast_url=%s\nlast_place=%d\n", g_cfg.last_name, g_cfg.last_url, g_cfg.last_place);
	fprintf(f, "spk_lid2=%d\n", g_cfg.spk_lid);   // new key: the old "spk_lid=1" (on by default) is ignored
	fclose(f);
}

void favs_save(void)
{
	FILE *f = fopen(APP_DIR "/favorites.tsv", "w");
	if (!f) return;
	for (int i = 0; i < g_nfavs; i++)
		fprintf(f, "%s\t%s\t%d\n", g_favs[i].name, g_favs[i].url, g_favs[i].place);
	fclose(f);
}

int fav_find(const char *url)
{
	for (int i = 0; i < g_nfavs; i++)
		if (!strcmp(g_favs[i].url, url)) return i;
	return -1;
}

bool fav_toggle(const char *name, const char *url, int place)
{
	int i = fav_find(url);
	if (i >= 0) {
		memmove(&g_favs[i], &g_favs[i + 1], (g_nfavs - i - 1) * sizeof(Fav));
		g_nfavs--;
		if (g_cfg.alarm_src == i) g_cfg.alarm_src = -1;
		else if (g_cfg.alarm_src > i) g_cfg.alarm_src--;
		favs_save();
		config_save();
		return false;
	}
	if (g_nfavs >= MAX_FAVS) return false;
	Fav *fv = &g_favs[g_nfavs++];
	snprintf(fv->name, sizeof fv->name, "%s", name);
	snprintf(fv->url, sizeof fv->url, "%s", url);
	fv->place = place;
	favs_save();
	return true;
}
