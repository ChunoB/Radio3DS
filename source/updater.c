// In-app updater: checks the latest GitHub release and installs it over Wi-Fi.
// Running as a .cia it streams the new .cia straight into the system installer (AM);
// running as a .3dsx it replaces the .3dsx file on the SD card.
// Shared by all the apps: only the repo, version and file names change.
#include "updater.h"
#include <3ds.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CA_FILE "romfs:/cacert.pem"

static char g_repo[64], g_version[16], g_3dsx_name[64], g_3dsx_path[256];
static volatile UpdState g_state = UPD_IDLE;
static char g_latest[32], g_url_cia[512], g_url_3dsx[512], g_msg[128];
static volatile float g_prog;
static Thread g_thread;
static volatile bool g_busy;

void updater_init(const char *repo, const char *version, const char *name_3dsx, const char *path_3dsx)
{
	snprintf(g_repo, sizeof g_repo, "%s", repo);
	snprintf(g_version, sizeof g_version, "%s", version);
	snprintf(g_3dsx_name, sizeof g_3dsx_name, "%s", name_3dsx);
	if (path_3dsx && !strncmp(path_3dsx, "sdmc:/", 6)) snprintf(g_3dsx_path, sizeof g_3dsx_path, "%s", path_3dsx);
	else snprintf(g_3dsx_path, sizeof g_3dsx_path, "sdmc:/3ds/%s", name_3dsx);
}

UpdState    updater_state(void) { return g_state; }
float       updater_progress(void) { return g_prog; }
const char *updater_latest(void) { return g_latest; }
const char *updater_message(void) { return g_msg; }
const char *updater_version(void) { return g_version; }
bool        updater_is_cia(void) { return !envIsHomebrew(); }

// "v1.2.3" or "1.2.3" -> comparable number
static long ver_num(const char *s)
{
	if (*s == 'v' || *s == 'V') s++;
	int a = 0, b = 0, c = 0;
	sscanf(s, "%d.%d.%d", &a, &b, &c);
	return a * 1000000L + b * 1000L + c;
}

// ---------------------------------------------------------------- http helpers

typedef struct { char *buf; size_t len, cap; } Mem;

static size_t mem_write(char *p, size_t sz, size_t n, void *ud)
{
	Mem *m = ud;
	size_t k = sz * n;
	if (m->len + k + 1 > m->cap) {
		size_t nc = (m->len + k + 1) * 2;
		if (nc > 512 * 1024) return 0;   // release info is small; refuse anything huge
		char *nb = realloc(m->buf, nc);
		if (!nb) return 0;
		m->buf = nb;
		m->cap = nc;
	}
	memcpy(m->buf + m->len, p, k);
	m->len += k;
	m->buf[m->len] = 0;
	return k;
}

static int on_progress(void *ud, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un)
{
	if (total > 0) g_prog = (float)now / (float)total;
	return 0;
}

static CURL *new_curl(const char *url)
{
	CURL *c = curl_easy_init();
	if (!c) return NULL;
	char ua[160];
	snprintf(ua, sizeof ua, "%s-updater/%s (Nintendo 3DS)", g_3dsx_name, g_version);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_USERAGENT, ua);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(c, CURLOPT_CAINFO, CA_FILE);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 64L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64 * 1024L);
	return c;
}

// Finds "key":"value" after *pos in json; copies value, returns true on success.
static bool json_str(const char *json, const char *key, const char **pos, char *out, size_t n)
{
	char pat[64];
	snprintf(pat, sizeof pat, "\"%s\"", key);
	const char *p = strstr(*pos, pat);
	if (!p) return false;
	p += strlen(pat);
	while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	if (*p != '"') return false;
	p++;
	size_t o = 0;
	while (*p && *p != '"' && o + 1 < n) {
		if (*p == '\\' && p[1]) p++;
		out[o++] = *p++;
	}
	out[o] = 0;
	*pos = p;
	return true;
}

static void fail(const char *m)
{
	snprintf(g_msg, sizeof g_msg, "%s", m);
	g_state = UPD_ERROR;
}

// ---------------------------------------------------------------- check

static void check_thread(void *arg)
{
	char url[160];
	snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", g_repo);
	Mem m = {0};
	CURL *c = new_curl(url);
	if (!c) { fail("Sem memória"); g_busy = false; return; }
	struct curl_slist *h = curl_slist_append(NULL, "Accept: application/vnd.github+json");
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
	CURLcode r = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(h);
	curl_easy_cleanup(c);
	if (code == 404) {
		fail("Nenhuma versão publicada ainda");
		free(m.buf);
		g_busy = false;
		return;
	}
	if (r != CURLE_OK || !m.buf) {
		snprintf(g_msg, sizeof g_msg, "Não consegui procurar: %s", curl_easy_strerror(r));
		g_state = UPD_ERROR;
		free(m.buf);
		g_busy = false;
		return;
	}

	const char *pos = m.buf;
	char tag[32] = "";
	json_str(m.buf, "tag_name", &pos, tag, sizeof tag);
	g_url_cia[0] = g_url_3dsx[0] = 0;
	pos = m.buf;
	char u[512];
	while (json_str(m.buf, "browser_download_url", &pos, u, sizeof u)) {
		size_t l = strlen(u);
		if (l > 4 && !strcmp(u + l - 4, ".cia")) snprintf(g_url_cia, sizeof g_url_cia, "%s", u);
		if (l > 5 && !strcmp(u + l - 5, ".3dsx")) snprintf(g_url_3dsx, sizeof g_url_3dsx, "%s", u);
	}
	free(m.buf);

	if (!tag[0]) { fail("Nenhuma versão publicada ainda"); g_busy = false; return; }
	snprintf(g_latest, sizeof g_latest, "%s", tag[0] == 'v' ? tag + 1 : tag);
	if (ver_num(tag) <= ver_num(g_version)) {
		snprintf(g_msg, sizeof g_msg, "Você já tem a versão mais nova (%s)", g_version);
		g_state = UPD_UP_TO_DATE;
	} else if (updater_is_cia() ? !g_url_cia[0] : !g_url_3dsx[0]) {
		fail("A versão nova não tem o arquivo para este formato");
	} else {
		snprintf(g_msg, sizeof g_msg, "Versão %s disponível (você tem %s)", g_latest, g_version);
		g_state = UPD_AVAILABLE;
	}
	g_busy = false;
}

// ---------------------------------------------------------------- install

typedef struct { Handle h; u64 off; bool err; } CiaSink;

static size_t cia_write(char *p, size_t sz, size_t n, void *ud)
{
	CiaSink *s = ud;
	size_t k = sz * n;
	u32 w = 0;
	if (R_FAILED(FSFILE_Write(s->h, &w, s->off, p, k, 0)) || w != k) { s->err = true; return 0; }
	s->off += k;
	return k;
}

static size_t file_write(char *p, size_t sz, size_t n, void *ud)
{
	return fwrite(p, sz, n, (FILE *)ud) * sz;
}

static void install_thread(void *arg)
{
	g_prog = 0;
	if (updater_is_cia()) {
		if (R_FAILED(amInit())) { fail("Sem acesso ao instalador do sistema"); g_busy = false; return; }
		Handle h;
		// "overwrite" lets the same title be reinstalled over itself (like FBI does)
		if (R_FAILED(AM_StartCiaInstallOverwrite(&h, MEDIATYPE_SD)) && R_FAILED(AM_StartCiaInstall(MEDIATYPE_SD, &h))) {
			amExit();
			fail("O sistema recusou a instalação");
			g_busy = false;
			return;
		}
		CiaSink sink = { h, 0, false };
		CURL *c = new_curl(g_url_cia);
		CURLcode r = CURLE_OUT_OF_MEMORY;
		if (c) {
			curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cia_write);
			curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
			curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
			r = curl_easy_perform(c);
			curl_easy_cleanup(c);
		}
		if (r != CURLE_OK || sink.err) {
			AM_CancelCIAInstall(h);
			amExit();
			snprintf(g_msg, sizeof g_msg, "Falha no download: %s", sink.err ? "erro ao gravar" : curl_easy_strerror(r));
			g_state = UPD_ERROR;
			g_busy = false;
			return;
		}
		g_state = UPD_INSTALLING;
		Result fr = AM_FinishCiaInstall(h);
		amExit();
		if (R_FAILED(fr)) {
			snprintf(g_msg, sizeof g_msg, "O sistema não aceitou o arquivo (%08lX)", fr);
			g_state = UPD_ERROR;
			g_busy = false;
			return;
		}
	} else {
		char tmp[272];
		snprintf(tmp, sizeof tmp, "%s.novo", g_3dsx_path);
		FILE *f = fopen(tmp, "wb");
		if (!f) { fail("Não consegui gravar no cartão SD"); g_busy = false; return; }
		CURL *c = new_curl(g_url_3dsx);
		CURLcode r = CURLE_OUT_OF_MEMORY;
		if (c) {
			curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, file_write);
			curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
			curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
			r = curl_easy_perform(c);
			curl_easy_cleanup(c);
		}
		bool ok = fclose(f) == 0 && r == CURLE_OK;
		if (!ok) {
			remove(tmp);
			snprintf(g_msg, sizeof g_msg, "Falha no download: %s", curl_easy_strerror(r));
			g_state = UPD_ERROR;
			g_busy = false;
			return;
		}
		g_state = UPD_INSTALLING;
		remove(g_3dsx_path);
		if (rename(tmp, g_3dsx_path) != 0) { fail("Não consegui trocar o arquivo .3dsx"); g_busy = false; return; }
	}
	g_prog = 1;
	snprintf(g_msg, sizeof g_msg, "Versão %s instalada! Feche e abra o app.", g_latest);
	g_state = UPD_DONE;
	g_busy = false;
}

// ---------------------------------------------------------------- public

static void start(ThreadFunc fn, UpdState st)
{
	if (g_busy) return;
	if (g_thread) { threadJoin(g_thread, U64_MAX); threadFree(g_thread); g_thread = NULL; }
	g_busy = true;
	g_state = st;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// below the APT thread (0x31) so the HOME button never waits behind us
	g_thread = threadCreate(fn, NULL, 64 * 1024, prio + 2, -2, false);
	if (!g_thread) { g_busy = false; fail("Falha ao criar thread"); }
}

void updater_check(void)
{
	snprintf(g_msg, sizeof g_msg, "Procurando atualização...");
	start(check_thread, UPD_CHECKING);
}

void updater_install(void)
{
	if (g_state != UPD_AVAILABLE) return;
	snprintf(g_msg, sizeof g_msg, "Baixando a versão %s...", g_latest);
	start(install_thread, UPD_DOWNLOADING);
}

bool updater_busy(void) { return g_busy; }

void updater_exit(void)
{
	if (g_thread) {
		threadJoin(g_thread, U64_MAX);
		threadFree(g_thread);
		g_thread = NULL;
	}
}
