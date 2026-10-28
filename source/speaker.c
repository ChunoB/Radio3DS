// Experimental: keep the speaker on with the lid closed.
//
// When the lid closes, the system's CODEC service mutes the speaker (headphones keep
// working). The 3DS codec registers are not documented, so this works by comparison:
// while the lid is open we keep a snapshot of the speaker-related register pages, and
// when the lid closes while something is playing we write back the registers that the
// system changed. Everything it sees and does goes to sdmc:/3ds/Radio3DS/codec.log.
#include "common.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define LOG_PATH APP_DIR "/codec.log"
#define NPAGES 3
static const u8   pages[NPAGES]    = { 0x00, 0x01, 0x64 };
static const bool writable[NPAGES] = { false, true, true };   // page 0 is only logged

static bool ok, have_open, audio;
static u8   open_regs[NPAGES][128], volat[NPAGES][128];
static u64  opened_at, closed_since, last_read;
static bool was_closed, logged_close, wrote_this_close;
static int  fixes, closes_seen, closes_without_diff;
static char status[96] = "Aguardando a tampa fechar";

static void codec_log(const char *fmt, ...)
{
	FILE *f = fopen(LOG_PATH, "a");
	if (!f) return;
	time_t t = time(NULL);
	struct tm *lt = localtime(&t);
	fprintf(f, "%02d:%02d:%02d ", lt->tm_hour, lt->tm_min, lt->tm_sec);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static Result rd(u8 page, u8 reg, u8 *out, size_t n)
{
	Result r = CDCCHK_ReadRegisters2(page, reg, out, n);
	if (R_FAILED(r)) r = CDCCHK_ReadRegisters1(page, reg, out, n);
	return r;
}

static Result wr(u8 page, u8 reg, u8 val)
{
	Result r = CDCCHK_WriteRegisters2(page, reg, &val, 1);
	if (R_FAILED(r)) r = CDCCHK_WriteRegisters1(page, reg, &val, 1);
	return r;
}

static bool read_all(u8 out[NPAGES][128])
{
	for (int p = 0; p < NPAGES; p++)
		for (int h = 0; h < 128; h += 64)
			if (R_FAILED(rd(pages[p], h, &out[p][h], 64))) return false;
	return true;
}

static bool headphones(void)
{
	// with audio shut down the answer is unknown: treat it as "headphones in" so no
	// snapshot is taken in a state we can't trust
	bool in = false;
	if (!audio || !player_audio_on() || R_FAILED(DSP_GetHeadphoneStatus(&in))) return true;
	return in;
}

void speaker_init(bool audio_ok)
{
	audio = audio_ok;
	struct stat st;
	if (stat(LOG_PATH, &st) == 0 && st.st_size > 128 * 1024) remove(LOG_PATH);

	// asking for a service that isn't registered blocks forever by default
	srvSetBlockingPolicy(true);
	Result r = cdcChkInit();
	srvSetBlockingPolicy(false);
	if (R_FAILED(r)) {
		snprintf(status, sizeof status, "Indisponível: sem acesso ao codec");
		codec_log("cdcChkInit falhou: %08lX", r);
		return;
	}
	u8 test[4];
	r = rd(1, 0, test, 4);
	if (R_FAILED(r)) {
		snprintf(status, sizeof status, "Indisponível: codec não respondeu");
		codec_log("leitura do codec falhou: %08lX", r);
		cdcChkExit();
		return;
	}
	ok = true;
	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);
	codec_log("--- iniciado (%s 3DS)", n3ds ? "New" : "Old");
}

void speaker_exit(void)
{
	if (ok) cdcChkExit();
	ok = false;
}

const char *speaker_status(void) { return status; }

// Takes the open-lid snapshot in two steps on consecutive calls (no waiting on the
// main thread). Returns true when a snapshot was completed.
static bool snapshot_open(void)
{
	static u8 a[NPAGES][128], b[NPAGES][128];
	static bool have_a;
	if (!have_a) {
		have_a = read_all(a);
		return false;
	}
	have_a = false;
	if (!read_all(b)) return false;
	// registers that change by themselves (status flags, counters) are never touched
	for (int p = 0; p < NPAGES; p++)
		for (int i = 0; i < 128; i++)
			if (a[p][i] != b[p][i]) volat[p][i] = 1;
	if (!have_open) codec_log("referência com a tampa aberta salva");
	memcpy(open_regs, b, sizeof open_regs);
	have_open = true;
	return true;
}

static void log_diffs(const u8 cur[NPAGES][128])
{
	int n = 0;
	for (int p = 0; p < NPAGES; p++)
		for (int i = 1; i < 128; i++)
			if (cur[p][i] != open_regs[p][i]) {
				codec_log("  pág %02X reg %02X: aberta %02X fechada %02X%s", pages[p], i, open_regs[p][i], cur[p][i],
				     volat[p][i] ? " (muda sozinho)" : "");
				n++;
			}
	if (!n) codec_log("  nenhuma diferença nos registradores");
}

void speaker_update(bool lid_closed, bool want_sound, u64 now)
{
	if (!ok) return;

	if (!lid_closed) {
		if (was_closed) {
			was_closed = false;
			opened_at = now;
			codec_log("tampa aberta");
		}
		// refresh the reference every 10 s, once the system has settled after opening;
		// a snapshot that is half done finishes on the very next call
		static bool mid;
		if (!g_cfg.spk_lid || now - opened_at < 2000) return;
		if (!mid && now - last_read < (have_open ? 10000u : 500u)) return;
		last_read = now;
		if (!mid && headphones()) return;
		mid = !snapshot_open() && !mid;
		return;
	}

	if (!was_closed) {
		was_closed = true;
		closed_since = now;
		logged_close = wrote_this_close = false;
		closes_seen++;
		codec_log("tampa fechada (tocando: %s)", want_sound ? "sim" : "não");
	}
	if (!g_cfg.spk_lid || now - closed_since < 800 || now - last_read < 1000) return;
	last_read = now;
	if (!have_open) {
		if (!logged_close) codec_log("sem referência da tampa aberta ainda");
		logged_close = true;
		return;
	}
	if (headphones()) return;

	static u8 cur[NPAGES][128];
	if (!read_all(cur)) return;
	if (!logged_close) {
		codec_log("diferenças com a tampa fechada:");
		log_diffs((const u8 (*)[128])cur);
		logged_close = true;
	}
	if (!want_sound) return;

	int n = 0, fails = 0;
	for (int p = 0; p < NPAGES; p++) {
		if (!writable[p]) continue;
		for (int i = 1; i < 128; i++) {
			if (volat[p][i] || cur[p][i] == open_regs[p][i]) continue;
			if (R_SUCCEEDED(wr(pages[p], i, open_regs[p][i]))) n++;
			else fails++;
		}
	}
	if (n || fails) {
		fixes++;
		codec_log("religando alto-falante: %d registradores escritos, %d falhas", n, fails);
		wrote_this_close = true;
		snprintf(status, sizeof status, "Religou o alto-falante %d vez%s", fixes, fixes == 1 ? "" : "es");
	} else if (!wrote_this_close) {
		closes_without_diff++;
		if (closes_without_diff == 1) codec_log("nada para religar nas páginas 01/64");
		snprintf(status, sizeof status, "Nada para religar (veja codec.log)");
	}
}
