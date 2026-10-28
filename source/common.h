#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>
#include <stddef.h>

#define APP_DIR "sdmc:/3ds/Radio3DS"

// ---------- station database (db.c) ----------
typedef struct {
	const char *name, *country;
	float lat, lon;              // degrees
	float slat, clat, slon, clon;
	short tz_std, tz_dst;        // minutes from UTC
	int first, count;            // range in g_stations
} Place;

typedef struct {
	const char *name, *url, *tags;
	int bitrate;
	int place;
} Station;

extern Place   *g_places;   extern int g_nplaces;
extern Station *g_stations; extern int g_nstations;

bool db_load(void);
int  db_nearest_place(float lat, float lon, float *out_dot);
int  place_utc_offset(const Place *p, time_t utc);   // minutes, with rough DST

// ---------- favorites + settings (config.c) ----------
typedef struct {
	char name[64];
	char url[512];
	int  place;      // -1 = unknown
} Fav;

#define MAX_FAVS 200

typedef struct {
	int   volume;          // 0..100
	int   tz;              // user's UTC offset in minutes
	bool  alarm_on;
	int   alarm_h, alarm_m;
	int   alarm_src;       // -1 = last station, otherwise favorite index
	float view_lat, view_lon, zoom;
	char  last_name[64];
	char  last_url[512];
	int   last_place;
	bool  spk_lid;         // try to keep the speaker on with the lid closed
} Config;

extern Config g_cfg;
extern Fav    g_favs[MAX_FAVS];
extern int    g_nfavs;

void config_load(void);
void config_save(void);
void favs_save(void);
int  fav_find(const char *url);
bool fav_toggle(const char *name, const char *url, int place);  // returns new state

// ---------- audio player (player.c) ----------
typedef enum { PL_STOPPED, PL_CONNECTING, PL_BUFFERING, PL_PLAYING, PL_ERROR } PlayerState;

bool        player_init(void);           // false if DSP firmware missing
void        player_exit(void);
void        player_play(const char *url);
void        player_stop(void);
PlayerState player_state(void);
void        player_title(char *out, size_t n);   // ICY song title ("" if none)
void        player_error(char *out, size_t n);
int         player_kbps(void);
void        player_set_volume(float v);          // 0..1
void        player_spectrum(float *bands, int n); // 0..1 each, call once per frame
void        player_tone(bool on);                // alarm beep fallback
bool        player_suspend(void);                // before HOME: stop and shut NDSP down (returns tone state)
bool        player_resume(void);                 // after HOME: bring NDSP back
bool        player_audio_on(void);               // is NDSP running?

// ---------- speaker with the lid closed, experimental (speaker.c) ----------
void        speaker_init(bool audio_ok);
void        speaker_exit(void);
void        speaker_update(bool lid_closed, bool want_sound, u64 now_ms);
const char *speaker_status(void);

// ---------- text helpers (gfx.c) ----------
void  text_init(void);
void  text_exit(void);
void  text_frame(void);   // clear glyph buffer, once per frame
float text_width(const char *s, float scale);
// align: 0 left, 1 center, 2 right (x is the anchor)
void  text_draw(const char *s, float x, float y, float scale, u32 color, float maxw, int align);
// Splits s into up to 2 lines that fit maxw; returns line count
int   text_wrap2(const char *s, float scale, float maxw, char *l1, char *l2, size_t n);
void  utf8_sanitize(char *s);   // turns stray Latin-1 bytes into UTF-8 in place (s must have room x2)
void  fold_text(const char *in, char *out, size_t n); // lowercase + strip accents, for search

// ---------- globe + top screen (globe.c) ----------
typedef struct {
	float lat, lon, zoom;
	int   focus;          // focused place index
	int   playing_place;  // -1 none
	bool  playing, buffering, error;
	time_t now_local;     // console clock
	int   user_tz;
	bool  alarm_on; int alarm_h, alarm_m;
} GlobeView;

void globe_init(void);
void globe_draw(C3D_RenderTarget *t, const GlobeView *v, float eye, float slider);
void clock_draw(C3D_RenderTarget *t, const GlobeView *v, float eye, float slider,
                const char *line1, const char *line2);
