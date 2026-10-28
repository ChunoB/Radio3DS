// Internet radio playback: a network thread (curl) fills a ring buffer with
// MP3 bytes (ICY metadata stripped out), a decoder thread (mpg123) turns them
// into PCM wave buffers for the DSP.
#define _GNU_SOURCE   // strcasestr
#include "common.h"
#include <curl/curl.h>
#include <mpg123.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

#define RING_SIZE   (192 * 1024)
#define NBUF        12
#define BUF_FRAMES  4096
#define START_BUFS  5
#define PL_MAX      (16 * 1024)
#define CH_MUSIC    0
#define CH_TONE     1

typedef struct {
	volatile bool cancel;
	volatile int refs;
	char url[512];

	LightLock lk;               // guards ring + title + err
	u8 *ring;
	size_t rhead, rcount;
	volatile bool net_done;
	volatile u32 total_rx;
	char title[256];
	char err[96];
	volatile PlayerState state;
	volatile int kbps;

	// network thread only
	int metaint, until_meta, meta_len, meta_pos, mstate;
	char meta[4096 + 16];
	bool playlist_mode, first_chunk;
	char *pl;
	size_t pl_len;
} Session;

static Session *g_cur;
static Thread g_dec;
static bool g_ok, g_ndsp;

static ndspWaveBuf g_wb[NBUF];
static s16 *g_pcm;
static volatile int g_channels = 2;
static LightLock g_audio_lock;
static volatile float g_volume = 0.8f;

static s16 *g_tone_pcm;
static ndspWaveBuf g_tone_wb;
static bool g_tone_on;

// ---------------------------------------------------------------- helpers

static void sess_release(Session *s)
{
	if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST) == 0) {
		free(s->ring);
		free(s->pl);
		free(s);
	}
}

static void sess_err(Session *s, const char *msg)
{
	LightLock_Lock(&s->lk);
	if (!s->err[0]) snprintf(s->err, sizeof s->err, "%s", msg);
	LightLock_Unlock(&s->lk);
}

static size_t ring_write(Session *s, const u8 *d, size_t n)
{
	LightLock_Lock(&s->lk);
	size_t space = RING_SIZE - s->rcount;
	if (n > space) n = space;
	size_t tail = (s->rhead + s->rcount) % RING_SIZE;
	size_t a = RING_SIZE - tail;
	if (a > n) a = n;
	memcpy(s->ring + tail, d, a);
	memcpy(s->ring, d + a, n - a);
	s->rcount += n;
	LightLock_Unlock(&s->lk);
	return n;
}

static size_t ring_read(Session *s, u8 *d, size_t n)
{
	LightLock_Lock(&s->lk);
	if (n > s->rcount) n = s->rcount;
	size_t a = RING_SIZE - s->rhead;
	if (a > n) a = n;
	memcpy(d, s->ring + s->rhead, a);
	memcpy(d + a, s->ring, n - a);
	s->rhead = (s->rhead + n) % RING_SIZE;
	s->rcount -= n;
	LightLock_Unlock(&s->lk);
	return n;
}

static size_t ring_count(Session *s)
{
	LightLock_Lock(&s->lk);
	size_t n = s->rcount;
	LightLock_Unlock(&s->lk);
	return n;
}

static void apply_mix(void)
{
	float mix[12] = {0};
	mix[0] = mix[1] = g_volume;
	ndspChnSetMix(CH_MUSIC, mix);
	float tv = g_volume < 0.6f ? 0.6f : g_volume;
	mix[0] = mix[1] = tv;
	ndspChnSetMix(CH_TONE, mix);
}

// ---------------------------------------------------------------- network

static bool push_audio(Session *s, const u8 *d, size_t n)
{
	size_t done = 0;
	while (done < n) {
		if (s->cancel) return false;
		size_t w = ring_write(s, d + done, n - done);
		done += w;
		s->total_rx += w;
		if (done < n) svcSleepThread(10 * 1000000LL);
	}
	return true;
}

static void title_from_meta(Session *s)
{
	char *p = strstr(s->meta, "StreamTitle='");
	if (!p) return;
	p += 13;
	char *e = strstr(p, "';");
	if (!e) e = strrchr(p, '\'');
	if (!e) e = p + strlen(p);
	char tmp[520];
	size_t n = e - p;
	if (n > 255) n = 255;
	memcpy(tmp, p, n);
	tmp[n] = 0;
	utf8_sanitize(tmp);
	LightLock_Lock(&s->lk);
	snprintf(s->title, sizeof s->title, "%s", tmp);
	LightLock_Unlock(&s->lk);
}

static size_t write_cb(char *ptr, size_t sz, size_t nm, void *ud)
{
	Session *s = ud;
	size_t n = sz * nm;
	const u8 *d = (const u8 *)ptr;
	if (s->cancel) return 0;

	if (s->first_chunk && n > 0) {
		s->first_chunk = false;
		if (!strncmp(ptr, "#EXTM3U", n < 7 ? n : 7) || (n >= 10 && !strncasecmp(ptr, "[playlist]", 10)) ||
		    (n >= 4 && !strncmp(ptr, "http", 4)))
			s->playlist_mode = true;
		else if (n >= 5 && (!strncasecmp(ptr, "<!DOC", 5) || !strncasecmp(ptr, "<html", 5))) {
			sess_err(s, "Endereço é uma página web, não áudio");
			return 0;
		}
	}

	if (s->playlist_mode) {
		if (!s->pl) s->pl = malloc(PL_MAX + 1);
		if (!s->pl) return 0;
		size_t room = PL_MAX - s->pl_len;
		size_t c = n < room ? n : room;
		memcpy(s->pl + s->pl_len, ptr, c);
		s->pl_len += c;
		s->pl[s->pl_len] = 0;
		return n;
	}

	if (s->metaint <= 0) return push_audio(s, d, n) ? n : 0;

	size_t i = 0;
	while (i < n) {
		if (s->mstate == 0) {
			size_t take = n - i;
			if (take > (size_t)s->until_meta) take = s->until_meta;
			if (take && !push_audio(s, d + i, take)) return 0;
			i += take;
			s->until_meta -= take;
			if (s->until_meta == 0) s->mstate = 1;
		} else if (s->mstate == 1) {
			s->meta_len = d[i++] * 16;
			s->meta_pos = 0;
			if (s->meta_len == 0) { s->mstate = 0; s->until_meta = s->metaint; }
			else s->mstate = 2;
		} else {
			size_t take = n - i;
			if (take > (size_t)(s->meta_len - s->meta_pos)) take = s->meta_len - s->meta_pos;
			memcpy(s->meta + s->meta_pos, d + i, take);
			s->meta_pos += take;
			i += take;
			if (s->meta_pos == s->meta_len) {
				s->meta[s->meta_len] = 0;
				title_from_meta(s);
				s->mstate = 0;
				s->until_meta = s->metaint;
			}
		}
	}
	return n;
}

static size_t header_cb(char *b, size_t sz, size_t nm, void *ud)
{
	Session *s = ud;
	size_t n = sz * nm;
	char line[256];
	size_t m = n < 255 ? n : 255;
	memcpy(line, b, m);
	line[m] = 0;
	while (m && (line[m - 1] == '\r' || line[m - 1] == '\n')) line[--m] = 0;

	if (!strncmp(line, "HTTP/", 5) || !strncmp(line, "ICY", 3)) {
		s->metaint = 0;
		s->mstate = 0;
		s->playlist_mode = false;
	} else if (!strncasecmp(line, "icy-metaint:", 12)) {
		s->metaint = atoi(line + 12);
		s->until_meta = s->metaint;
		s->mstate = 0;
	} else if (!strncasecmp(line, "content-type:", 13)) {
		const char *v = line + 13;
		if (strcasestr(v, "mpegurl") || strcasestr(v, "scpls") || strcasestr(v, "x-pls"))
			s->playlist_mode = true;
	}
	return n;
}

static int xfer_cb(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
	return ((Session *)ud)->cancel ? 1 : 0;
}

static bool playlist_first_url(const char *pl, char *out, size_t n)
{
	const char *p = pl;
	while (*p) {
		const char *e = strpbrk(p, "\r\n");
		size_t len = e ? (size_t)(e - p) : strlen(p);
		const char *h = NULL;
		for (size_t i = 0; i + 7 < len; i++)
			if (!strncmp(p + i, "http://", 7) || !strncmp(p + i, "https://", 8)) { h = p + i; break; }
		if (h && p[0] != '#') {
			size_t l = len - (h - p);
			if (l >= n) l = n - 1;
			memcpy(out, h, l);
			out[l] = 0;
			while (l && (out[l - 1] == ' ' || out[l - 1] == '\t')) out[--l] = 0;
			return true;
		}
		if (!e) break;
		p = e + 1;
	}
	return false;
}

static void net_thread(void *arg)
{
	Session *s = arg;
	char url[512];
	snprintf(url, sizeof url, "%s", s->url);
	int fails = 0, hops = 0;
	CURL *c = curl_easy_init();
	struct curl_slist *hdr = curl_slist_append(NULL, "Icy-MetaData: 1");

	while (c && !s->cancel) {
		s->metaint = 0;
		s->mstate = 0;
		s->first_chunk = true;
		s->playlist_mode = false;
		s->pl_len = 0;
		u32 rx_before = s->total_rx;

		curl_easy_reset(c);
		curl_easy_setopt(c, CURLOPT_URL, url);
		curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
		curl_easy_setopt(c, CURLOPT_USERAGENT, "Radio3DS/1.0");
		curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(c, CURLOPT_MAXREDIRS, 6L);
		curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 12L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 256L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 15L);
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);   // no CA bundle on the console
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
		curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 16384L);
		curl_easy_setopt(c, CURLOPT_HTTP09_ALLOWED, 1L);   // old SHOUTcast "ICY 200 OK"
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
		curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
		curl_easy_setopt(c, CURLOPT_HEADERDATA, s);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, s);

		CURLcode r = curl_easy_perform(c);
		if (s->cancel) break;

		if (s->playlist_mode && s->pl_len) {
			if (++hops > 3 || !playlist_first_url(s->pl, url, sizeof url)) {
				sess_err(s, "Lista de reprodução inválida");
				break;
			}
			continue;
		}

		if (s->total_rx - rx_before > 64 * 1024) fails = 0;
		else fails++;
		if (fails >= (s->total_rx ? 4 : 2)) {
			char msg[96];
			long code = 0;
			curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
			switch (r) {
			case CURLE_COULDNT_RESOLVE_HOST: snprintf(msg, sizeof msg, "Sem internet ou endereço inválido"); break;
			case CURLE_COULDNT_CONNECT:      snprintf(msg, sizeof msg, "A rádio não respondeu"); break;
			case CURLE_OPERATION_TIMEDOUT:   snprintf(msg, sizeof msg, "Tempo esgotado"); break;
			case CURLE_HTTP_RETURNED_ERROR:  snprintf(msg, sizeof msg, "Rádio fora do ar (HTTP %ld)", code); break;
			case CURLE_OK:                   snprintf(msg, sizeof msg, "A transmissão terminou"); break;
			default: snprintf(msg, sizeof msg, "Erro de rede: %s", curl_easy_strerror(r)); break;
			}
			sess_err(s, msg);
			break;
		}
		for (int i = 0; i < 10 && !s->cancel; i++) svcSleepThread(100 * 1000000LL);
	}

	curl_slist_free_all(hdr);
	if (c) curl_easy_cleanup(c);
	else sess_err(s, "Falha ao iniciar a rede");
	s->net_done = true;
	sess_release(s);
}

// ---------------------------------------------------------------- decoder

static void chan_setup(int rate, int ch)
{
	LightLock_Lock(&g_audio_lock);
	ndspChnReset(CH_MUSIC);
	ndspChnSetInterp(CH_MUSIC, NDSP_INTERP_LINEAR);
	ndspChnSetRate(CH_MUSIC, (float)rate);
	ndspChnSetFormat(CH_MUSIC, ch == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	ndspChnSetPaused(CH_MUSIC, true);
	memset(g_wb, 0, sizeof g_wb);
	g_channels = ch;
	apply_mix();
	LightLock_Unlock(&g_audio_lock);
}

static int active_bufs(void)
{
	int n = 0;
	for (int i = 0; i < NBUF; i++)
		if (g_wb[i].status == NDSP_WBUF_QUEUED || g_wb[i].status == NDSP_WBUF_PLAYING) n++;
	return n;
}

static void submit(int idx, size_t bytes, int ch)
{
	ndspWaveBuf *wb = &g_wb[idx];
	s16 *buf = g_pcm + idx * BUF_FRAMES * 2;
	memset(wb, 0, sizeof *wb);
	wb->data_vaddr = buf;
	wb->nsamples = bytes / (ch * 2);
	DSP_FlushDataCache(buf, bytes);
	ndspChnWaveBufAdd(CH_MUSIC, wb);
}

static void dec_thread(void *arg)
{
	Session *s = arg;
	int err = 0;
	mpg123_handle *mh = mpg123_new(NULL, &err);
	u8 *in = malloc(8192);
	if (!mh || !in) {
		sess_err(s, "Sem memória para o decodificador");
		s->state = PL_ERROR;
		if (mh) mpg123_delete(mh);
		free(in);
		return;
	}
	mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
	mpg123_format_none(mh);
	const long *rates;
	size_t nr;
	mpg123_rates(&rates, &nr);
	for (size_t i = 0; i < nr; i++)
		mpg123_format(mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
	mpg123_open_feed(mh);

	bool configured = false, paused = true;
	int channels = 2;
	long rate = 0;
	int cur = -1;
	size_t filled = 0;
	int errors = 0;
	int info_tick = 0;

	while (!s->cancel) {
		int active = configured ? active_bufs() : 0;
		if (configured && !paused && active == 0) {
			ndspChnSetPaused(CH_MUSIC, true);
			paused = true;
			s->state = PL_BUFFERING;
		}
		if (configured && paused &&
		    (active >= START_BUFS || (active > 0 && (s->net_done || (ring_count(s) == 0 && cur < 0))))) {
			ndspChnSetPaused(CH_MUSIC, false);
			paused = false;
			s->state = PL_PLAYING;
		}

		if (cur < 0) {
			for (int i = 0; i < NBUF; i++)
				if (g_wb[i].status == NDSP_WBUF_FREE || g_wb[i].status == NDSP_WBUF_DONE) { cur = i; break; }
			filled = 0;
			if (cur < 0) { svcSleepThread(4 * 1000000LL); continue; }
		}

		u8 *buf = (u8 *)(g_pcm + cur * BUF_FRAMES * 2);
		size_t cap = BUF_FRAMES * channels * 2;
		size_t done = 0;
		// This thread runs above the UI. With plenty of audio queued, decode about one
		// MP3 frame at a time and nap in between, so the screen and the HOME button
		// never wait behind a burst of decoding. When the queue is low, go full speed.
		bool relaxed = configured && !paused && active >= 5;
		size_t want = cap - filled;
		if (relaxed && want > 1152u * channels * 2) want = 1152u * channels * 2;
		int r = mpg123_read(mh, buf + filled, want, &done);
		filled += done;
		if (relaxed && done) svcSleepThread(active >= 8 ? 16 * 1000000LL : 4 * 1000000LL);

		if (r == MPG123_NEW_FORMAT) {
			long rt; int ch, enc;
			mpg123_getformat(mh, &rt, &ch, &enc);
			if (!configured || rt != rate || ch != channels) {
				chan_setup(rt, ch);
				rate = rt;
				channels = ch;
				configured = true;
				paused = true;
				cur = -1;
				filled = 0;
				if (s->state == PL_CONNECTING || s->state == PL_PLAYING) s->state = PL_BUFFERING;
			}
			continue;
		}

		if (r == MPG123_NEED_MORE) {
			size_t got = ring_read(s, in, 8192);
			if (got) {
				mpg123_feed(mh, in, got);
			} else if (s->net_done) {
				// stream over: play what is left, then stop
				if (configured && filled >= (size_t)(channels * 2)) submit(cur, filled - filled % (channels * 2), channels);
				cur = -1;
				if (configured && paused && active_bufs() > 0) ndspChnSetPaused(CH_MUSIC, false);
				while (!s->cancel && configured && active_bufs() > 0) svcSleepThread(20 * 1000000LL);
				sess_err(s, "A transmissão terminou");   // keeps an earlier, more specific error
				s->state = PL_ERROR;
				break;
			} else {
				svcSleepThread(10 * 1000000LL);
			}
		} else if (r == MPG123_ERR) {
			if (++errors > 400) { sess_err(s, "Formato de áudio não suportado"); s->state = PL_ERROR; break; }
		}

		if (configured && cur >= 0 && filled >= cap) {
			submit(cur, filled, channels);
			cur = -1;
			filled = 0;
			if (++info_tick % 32 == 1) {
				struct mpg123_frameinfo fi;
				if (mpg123_info(mh, &fi) == MPG123_OK) s->kbps = fi.bitrate;
			}
		}
	}

	mpg123_delete(mh);
	free(in);
}

// ---------------------------------------------------------------- public

bool player_init(void)
{
	LightLock_Init(&g_audio_lock);
	if (R_FAILED(ndspInit())) return false;
	g_ndsp = true;
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	g_pcm = linearAlloc(NBUF * BUF_FRAMES * 2 * sizeof(s16));
	mpg123_init();

	// alarm beep: two short beeps per second, looped
	const int tr = 22050;
	g_tone_pcm = linearAlloc(tr * sizeof(s16));
	if (!g_pcm || !g_tone_pcm) return false;
	for (int i = 0; i < tr; i++) {
		float t = (float)i / tr;
		bool on = (t < 0.12f) || (t >= 0.20f && t < 0.32f);
		float env = on ? 1.0f : 0.0f;
		g_tone_pcm[i] = (s16)(env * 0.55f * 32767.0f * sinf(2 * (float)M_PI * 880.0f * t));
	}
	DSP_FlushDataCache(g_tone_pcm, tr * sizeof(s16));
	g_ok = true;
	return true;
}

// Going to the HOME menu with NDSP running can hang the whole console: libctru asks
// the DSP to suspend and waits forever for the answer. So before the jump we stop
// everything and shut NDSP down, and bring it back fresh afterwards.
// Returns whether the alarm tone was on, so the caller can turn it back on.
bool player_suspend(void)
{
	bool tone = g_tone_on;
	if (!g_ok) return false;
	player_stop();
	if (g_ndsp) {
		player_tone(false);
		ndspExit();
		g_ndsp = false;
	}
	return tone;
}

bool player_resume(void)
{
	if (!g_ok) return false;
	if (!g_ndsp) {
		if (R_FAILED(ndspInit())) return false;
		g_ndsp = true;
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		memset(g_wb, 0, sizeof g_wb);
		apply_mix();
	}
	return true;
}

void player_exit(void)
{
	if (!g_ok) { if (g_ndsp) ndspExit(); return; }
	player_stop();
	if (g_ndsp) {
		player_tone(false);
		ndspExit();
		g_ndsp = false;
	}
	linearFree(g_pcm);
	linearFree(g_tone_pcm);
	mpg123_exit();
	// network threads that are still running own their sessions; they exit on cancel.
}

bool player_audio_on(void) { return g_ndsp; }

void player_play(const char *url)
{
	if (!g_ok || !player_resume()) return;
	player_stop();
	Session *s = calloc(1, sizeof(Session));
	if (!s) return;
	s->ring = malloc(RING_SIZE);
	if (!s->ring) { free(s); return; }
	LightLock_Init(&s->lk);
	snprintf(s->url, sizeof s->url, "%s", url);
	s->state = PL_CONNECTING;
	s->refs = 2;   // network thread + us

	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	Thread nt = threadCreate(net_thread, s, 128 * 1024, prio - 1, -2, true);
	if (!nt) { free(s->ring); free(s); return; }
	g_dec = threadCreate(dec_thread, s, 64 * 1024, prio - 2, -2, false);
	g_cur = s;
	if (!g_dec) {
		sess_err(s, "Falha ao criar thread");
		s->state = PL_ERROR;
	}
}

void player_stop(void)
{
	Session *s = g_cur;
	if (!s) return;
	s->cancel = true;
	if (g_dec) {
		threadJoin(g_dec, U64_MAX);
		threadFree(g_dec);
		g_dec = NULL;
	}
	LightLock_Lock(&g_audio_lock);
	ndspChnReset(CH_MUSIC);
	memset(g_wb, 0, sizeof g_wb);
	LightLock_Unlock(&g_audio_lock);
	g_cur = NULL;
	sess_release(s);
}

PlayerState player_state(void)
{
	Session *s = g_cur;
	if (!s) return PL_STOPPED;
	PlayerState st = s->state;
	if (st == PL_CONNECTING && s->total_rx > 0) st = PL_BUFFERING;
	return st;
}

void player_title(char *out, size_t n)
{
	Session *s = g_cur;
	out[0] = 0;
	if (!s) return;
	LightLock_Lock(&s->lk);
	snprintf(out, n, "%s", s->title);
	LightLock_Unlock(&s->lk);
}

void player_error(char *out, size_t n)
{
	Session *s = g_cur;
	out[0] = 0;
	if (!s) return;
	LightLock_Lock(&s->lk);
	snprintf(out, n, "%s", s->err);
	LightLock_Unlock(&s->lk);
}

int player_kbps(void)
{
	return g_cur ? g_cur->kbps : 0;
}

void player_set_volume(float v)
{
	if (v < 0) v = 0;
	if (v > 1) v = 1;
	g_volume = v;
	if (g_ok && g_ndsp) apply_mix();
}

void player_tone(bool on)
{
	if (!g_ok || on == g_tone_on) return;
	if (on && !player_resume()) return;
	if (!on && !g_ndsp) { g_tone_on = false; return; }
	g_tone_on = on;
	if (on) {
		ndspChnReset(CH_TONE);
		ndspChnSetInterp(CH_TONE, NDSP_INTERP_LINEAR);
		ndspChnSetRate(CH_TONE, 22050.0f);
		ndspChnSetFormat(CH_TONE, NDSP_FORMAT_MONO_PCM16);
		apply_mix();
		memset(&g_tone_wb, 0, sizeof g_tone_wb);
		g_tone_wb.data_vaddr = g_tone_pcm;
		g_tone_wb.nsamples = 22050;
		g_tone_wb.looping = true;
		ndspChnWaveBufAdd(CH_TONE, &g_tone_wb);
	} else {
		ndspChnReset(CH_TONE);
	}
}

// ---------------------------------------------------------------- spectrum

#define FFT_N 256
static float fft_cos[FFT_N / 2], fft_sin[FFT_N / 2], hann[FFT_N];
static bool fft_ready;

static void fft(float *re, float *im)
{
	for (int i = 1, j = 0; i < FFT_N; i++) {
		int bit = FFT_N >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) {
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (int len = 2; len <= FFT_N; len <<= 1) {
		int step = FFT_N / len;
		for (int i = 0; i < FFT_N; i += len) {
			for (int k = 0; k < len / 2; k++) {
				float wr = fft_cos[k * step], wi = -fft_sin[k * step];
				int a = i + k, b = i + k + len / 2;
				float xr = re[b] * wr - im[b] * wi;
				float xi = re[b] * wi + im[b] * wr;
				re[b] = re[a] - xr; im[b] = im[a] - xi;
				re[a] += xr;        im[a] += xi;
			}
		}
	}
}

void player_spectrum(float *bands, int n)
{
	static float smooth[64];
	if (n > 64) n = 64;
	if (!fft_ready) {
		for (int i = 0; i < FFT_N / 2; i++) {
			fft_cos[i] = cosf(2 * (float)M_PI * i / FFT_N);
			fft_sin[i] = sinf(2 * (float)M_PI * i / FFT_N);
		}
		for (int i = 0; i < FFT_N; i++) hann[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (FFT_N - 1));
		fft_ready = true;
	}

	float raw[64] = {0};
	bool have = false;
	static float re[FFT_N], im[FFT_N];
	if (g_cur && g_cur->state == PL_PLAYING) {
		LightLock_Lock(&g_audio_lock);
		for (int i = 0; i < NBUF; i++) {
			ndspWaveBuf *wb = &g_wb[i];
			if (wb->status != NDSP_WBUF_PLAYING || wb->nsamples < FFT_N) continue;
			u32 pos = ndspChnGetSamplePos(CH_MUSIC);
			if (pos + FFT_N > wb->nsamples) pos = wb->nsamples - FFT_N;
			const s16 *p = (const s16 *)wb->data_vaddr;
			int ch = g_channels;
			for (int k = 0; k < FFT_N; k++) {
				float v = ch == 2 ? (p[(pos + k) * 2] + p[(pos + k) * 2 + 1]) * 0.5f : p[pos + k];
				re[k] = v * hann[k] / 32768.0f;
				im[k] = 0;
			}
			have = true;
			break;
		}
		LightLock_Unlock(&g_audio_lock);
	}
	if (have) {
		fft(re, im);
		int lo = 1;
		for (int b = 0; b < n; b++) {
			// log-ish spacing over bins 1..127
			int hi = 1 + (int)(126.0f * (powf(2.0f, 6.0f * (b + 1) / n) - 1.0f) / 63.0f);
			if (hi <= lo) hi = lo + 1;
			if (hi > 128) hi = 128;
			float m = 0;
			for (int k = lo; k < hi; k++) {
				float mag = sqrtf(re[k] * re[k] + im[k] * im[k]);
				if (mag > m) m = mag;
			}
			lo = hi < 127 ? hi : 127;
			float db = 20.0f * log10f(m / (FFT_N * 0.25f) + 1e-6f);
			db += 22.0f * b / (n - 1);   // tilt: music has much less energy up high
			float lvl = (db + 66.0f) / 62.0f;
			raw[b] = lvl < 0 ? 0 : lvl > 1 ? 1 : lvl;
		}
	}
	for (int b = 0; b < n; b++) {
		if (raw[b] > smooth[b]) smooth[b] = raw[b] * 0.7f + smooth[b] * 0.3f;
		else smooth[b] *= 0.90f;
		bands[b] = smooth[b];
	}
}
