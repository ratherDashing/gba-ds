/* GBA DS's emulator (src/emu.h) on armGBA instead of libmgba: the 3DS port's
 * core (third_party/armgba, its ARM recompiler on the console, its x86-64
 * one in the desktop test build). This file stands where src/emu.c does in
 * the other builds; the rest of GBA DS talks to it through emu.h alone.
 *
 * Each Emu is one armGBA machine (struct GBA): the game, the hidden copy the
 * map captures run on, a logo's. A state is armGBA's own (the machine as it
 * lies in memory), so one copy's state loads into another's directly.
 *
 * Not here yet (see 3ds/README.md): the HUD split (a second renderer fed the
 * same writes), rewind on the console, .7z archives. */
#include "gba.h"
#include "cheats.h"
#include "cpu/jit.h"
#ifdef __3DS__
#include "cpu/jit-internal.h"
#endif
#include "video/render.h"

#include "bench.h"
#include "emu.h"
#ifdef __3DS__
#include "host.h"
#endif
#include "patch.h"

#include <pthread.h>
#include <stdarg.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

/* What Emu.core points at (the other builds' struct mCore). */
typedef struct ArmCore {
	struct GBA* gba;
	struct CheatSet cheats;
	bool cheatsOn;
	/* the battery save's file (NULL: none), and how long the game's writes
	 * have been quiet */
	char* savePath;
	int saveQuiet;
	char warmPath[64]; /* (the game itself: its warm list's file) */
	bool late; /* (drawn on a render thread a frame late) */
	bool savePending;
#ifdef __3DS__
	int codeCore; /* the core that last wrote this machine's code: its shared routines are written where it is opened */
#endif
	/* armgba_stats: the lines' interval (0: none) and the counters at the last one (3DS) */
	unsigned statsEvery;
	struct JitSeen* seen;
	bool game; /* the machine with the sound: not a copy */
	uint64_t compileBudget; /* (3DS, the game: the host ticks of compiling a frame may take before the code met for the first times is put off) */
	unsigned ran, loads; /* frames run and states loaded here (a copy's frame counter is the game's, from the state it loaded) */
	/* sound not yet read (emu_audio_read): stereo frames in a ring */
	int16_t* ring;
	size_t ringCap, ringHead, ringCount;
	/* the ROM's buffer when another core lent it (shared_rom) */
	struct SharedRom* shared;
	unsigned layers;
	/* what its renderer leaves out (emu_layers, emu_hide_sprites) */
	struct RenderHooks hooks;
	SpriteRule rules[12];
	int nRules;
	bool keepOnly; /* hide every sprite that matches none of the rules instead */
	bool grouped;  /* a matching sprite hidden only as part of a group (emu_hide_grouped) */
	uint16_t exact[128][3];
	int nExact;
	bool exactKeep;
	/* emu_mark_backdrop: the same picture drawn again on a backdrop of
	 * MARK_555, where every pixel no layer or sprite drew stays MARK_555 */
	uint16_t* markFrame;
	/* armgba_picture_thread: the render workers widen the frames they hand over, into wideBuf in turn, and
	 * e->fb points at the one that holds the frame taken (fbOwn is the one this machine made) */
	uint32_t* fbOwn;
	uint32_t* wideBuf[2];
	bool wideLinear;   /* raw_picture: wideBuf is linear memory, written back by the worker, that the copy engine reads as it is */
	unsigned widePitch; /* ... and the words a row of it has (256: one transfer tiles it; 240: two passes) */
	uint32_t* twin[2]; /* gpu_twin: wideBuf's pictures again in the GPU's byte order (linear memory, HOST_TWIN_STRIDE words a row) */
	int bright; /* emu_bright_samples: the bright samples of the frame taken, as the workers counted them; -1 if not, or given */
} ArmCore;

static ArmCore* AC(const Emu* e) {
	return (ArmCore*) e->core;
}

uint32_t doCrc32(const void* buf, size_t size) {
	return (uint32_t) crc32(0, buf, (uInt) size);
}

/* ---------------------------------------------- ROM files */

/* A background thread's reading of a ROM can be given up part-way (a logo
 * being made when a game is started): the flag it set for itself. */
static _Thread_local const volatile bool* readCancel;
void emu_read_cancel(const volatile bool* flag) {
	readCancel = flag;
}

#ifdef __3DS__
#define rom_free host_rom_free
#else
#define rom_free free
#endif

#ifdef __3DS__
static _Thread_local uint8_t* romRoomNow; /* (the room emu_open set aside at the top for the read that comes next, and its size) */
static _Thread_local size_t romRoomSize;
#endif

/* top (3DS): read into a cartridge's whole buffer at the top of the heap's range, where a long session's
 * small allocations can't cut up the room the next game's cartridge needs. */
static uint8_t* read_file_to(const char* path, size_t* n, bool top) {
	struct stat st;
	*n = 0;
	if (stat(path, &st) || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) return NULL;
	FILE* f = fopen(path, "rb");
	uint8_t* p = NULL;
#ifdef __3DS__
	size_t const whole = gbaRomBufferSize((size_t) st.st_size);
	if (f && top && romRoomNow && romRoomSize == whole) p = romRoomNow, romRoomNow = NULL;
	else if (f && top) p = host_top_alloc(whole);
	if (p) memset(p + st.st_size, 0, whole - (size_t) st.st_size);
#else
	(void) top;
#endif
	if (f && !p) p = malloc((size_t) st.st_size);
#ifdef __3DS__
	if (!f) fprintf(stderr, "%s: can't be opened\n", path);
	else if (!p) {
		char shape[400];
		host_heap_shape(shape, sizeof(shape));
		fprintf(stderr, "%s: no memory for its %u KiB (%u KiB of the heap free): %s\n", path, (unsigned) (st.st_size >> 10), (unsigned) (host_heap_free() >> 10), shape);
	}
#endif
#ifdef __3DS__
	/* In pieces: one read of a whole ROM holds the SD card for seconds, and
	 * the game list's own reads (a logo, a thumbnail) wait behind it. */
	size_t got = 0;
	while (p && got < (size_t) st.st_size) {
		size_t want = (size_t) st.st_size - got;
		if (want > 64 * 1024) want = 64 * 1024;
		if (readCancel && *readCancel) break;
		size_t r = fread(p + got, 1, want, f);
		if (!r) {
			fprintf(stderr, "%s: read stopped at %u of %u bytes\n", path, (unsigned) got, (unsigned) st.st_size);
			break;
		}
		got += r;
		host_fs_yield();
	}
	if (p && got == (size_t) st.st_size) *n = got;
#else
	if (p && fread(p, 1, (size_t) st.st_size, f) == (size_t) st.st_size) *n = (size_t) st.st_size;
#endif
	else {
		rom_free(p);
		p = NULL;
	}
	if (f) fclose(f);
	return p;
}

static uint8_t* read_file(const char* path, size_t* n) {
	return read_file_to(path, n, false);
}

static bool has_ext(const char* name, const char* const* exts) {
	const char* dot = strrchr(name, '.');
	if (!dot) return false;
	for (int i = 0; exts[i]; ++i) {
		if (!strcasecmp(dot, exts[i])) return true;
	}
	return false;
}

/* .7z isn't read here (mGBA's reader did it in the other builds). */
bool emu_is_archive_name(const char* name) {
	static const char* const exts[] = { ".zip", NULL };
	return has_ext(name, exts);
}

bool emu_is_rom_name(const char* name) {
	static const char* const exts[] = { ".gba", ".zip", NULL };
	return has_ext(name, exts);
}

static uint32_t le32(const uint8_t* p) {
	return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}
static uint16_t le16(const uint8_t* p) {
	return (uint16_t) (p[0] | p[1] << 8);
}

/* The first .gba in a .zip: up to `want` bytes of it (0: all), malloc'd. */
static uint8_t* zip_first_rom(const char* path, size_t want, size_t* size) {
	*size = 0;
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	uint8_t* out = NULL;
	uint8_t* dir = NULL;
	uint8_t tail[22 + 1024];
	fseek(f, 0, SEEK_END);
	long fileSize = ftell(f);
	long tailN = fileSize < (long) sizeof(tail) ? fileSize : (long) sizeof(tail);
	if (tailN < 22 || fseek(f, fileSize - tailN, SEEK_SET) || fread(tail, 1, (size_t) tailN, f) != (size_t) tailN) goto done;
	long eocd = -1;
	for (long i = tailN - 22; i >= 0; --i) {
		if (le32(tail + i) == 0x06054B50) {
			eocd = i;
			break;
		}
	}
	if (eocd < 0) goto done;
	int entries = le16(tail + eocd + 10);
	uint32_t dirSize = le32(tail + eocd + 12), dirOff = le32(tail + eocd + 16);
	if (dirSize > 16 * 1024 * 1024 || !(dir = malloc(dirSize)) || fseek(f, (long) dirOff, SEEK_SET) ||
	    fread(dir, 1, dirSize, f) != dirSize) {
		goto done;
	}
	uint32_t at = 0;
	for (int i = 0; i < entries && at + 46 <= dirSize && le32(dir + at) == 0x02014B50; ++i) {
		int method = le16(dir + at + 10);
		uint32_t packed = le32(dir + at + 20), full = le32(dir + at + 24), local = le32(dir + at + 42);
		int nameLen = le16(dir + at + 28), extraLen = le16(dir + at + 30), commentLen = le16(dir + at + 32);
		const char* name = (const char*) dir + at + 46;
		at += 46 + (uint32_t) (nameLen + extraLen + commentLen);
		if (at > dirSize || nameLen < 4 || strncasecmp(name + nameLen - 4, ".gba", 4)) continue;
		if ((method != 0 && method != 8) || full == 0 || full > 64 * 1024 * 1024) continue;
		uint8_t lh[30];
		if (fseek(f, (long) local, SEEK_SET) || fread(lh, 1, 30, f) != 30 || le32(lh) != 0x04034B50) break;
		if (fseek(f, le16(lh + 26) + le16(lh + 28), SEEK_CUR)) break;
		size_t n = want && want < full ? want : full;
		out = malloc(n);
		if (!out) break;
		bool ok = false;
		if (method == 0) {
			ok = fread(out, 1, n, f) == n;
		} else {
			z_stream z;
			memset(&z, 0, sizeof(z));
			uint8_t in[16384];
			if (inflateInit2(&z, -MAX_WBITS) == Z_OK) {
				z.next_out = out;
				z.avail_out = (uInt) n;
				uint32_t left = packed;
				int r = Z_OK;
				while (z.avail_out && r == Z_OK) {
					if (!z.avail_in) {
						size_t got = fread(in, 1, left < sizeof(in) ? left : sizeof(in), f);
						if (!got) break;
						left -= (uint32_t) got;
						z.next_in = in;
						z.avail_in = (uInt) got;
					}
					r = inflate(&z, Z_NO_FLUSH);
				}
				ok = !z.avail_out;
				inflateEnd(&z);
			}
		}
		if (ok) *size = n;
		else {
			free(out);
			out = NULL;
		}
		break;
	}
done:
	free(dir);
	fclose(f);
	return out;
}

bool emu_rom_header(const char* path, uint8_t* out, size_t n) {
	if (n > EMU_HEADER_MAX) n = EMU_HEADER_MAX;
	if (emu_is_archive_name(path)) {
		size_t got = 0;
		uint8_t* p = zip_first_rom(path, n, &got);
		bool ok = p && got == n;
		if (ok) memcpy(out, p, n);
		free(p);
		return ok;
	}
#ifdef __3DS__
	return host_read_head(path, out, n);
#else
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	bool ok = fread(out, 1, n, f) == n;
	fclose(f);
	return ok;
#endif
}

uint8_t* emu_rom_read_all(const char* path, size_t* size) {
	if (emu_is_archive_name(path)) return zip_first_rom(path, 0, size);
	return read_file(path, size);
}

/* ---------------------------------------------- patches (GEN-9)
 * As src/emu.c has them: which patches the game applies (emu_patch_plan),
 * every core that opens that ROM getting them from one cached patched copy. */
#define PLAN_MAX 8
static pthread_mutex_t patchedLock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	char rom[1024];
	char files[PLAN_MAX][1024];
	int n;
} plan;
static struct {
	char key[PLAN_MAX * 1100 + 1100];
	int result;
	char names[256];
	char skipped[256];
	uint32_t fileCrc;
	uint8_t* data;
	size_t size;
} patched;

void emu_patch_plan(const char* rom, char files[][1024], int n) {
	pthread_mutex_lock(&patchedLock);
	snprintf(plan.rom, sizeof(plan.rom), "%s", rom ? rom : "");
	plan.n = 0;
	for (int i = 0; i < n && i < PLAN_MAX; ++i) snprintf(plan.files[plan.n++], sizeof(plan.files[0]), "%s", files[i]);
	pthread_mutex_unlock(&patchedLock);
}

static uint8_t* patched_with(const char* romPath, char files[][1024], int n, size_t* size, int* result, char* patchName,
                             uint32_t* fileCrc, char* skipped) {
	*result = PATCH_NONE;
	if (n > PLAN_MAX) n = PLAN_MAX;
	if (!n) return NULL;
	static char key[sizeof(patched.key)];
	struct stat st;
	pthread_mutex_lock(&patchedLock);
	int k = snprintf(key, sizeof(key), "%s|%lld|%lld", romPath, stat(romPath, &st) ? -1LL : (long long) st.st_size,
	                 stat(romPath, &st) ? -1LL : (long long) st.st_mtime);
	for (int i = 0; i < n && k < (int) sizeof(key); ++i) {
		bool ok = stat(files[i], &st) == 0;
		k += snprintf(key + k, sizeof(key) - (size_t) k, "|%s|%lld|%lld", files[i], ok ? (long long) st.st_size : -1LL,
		              ok ? (long long) st.st_mtime : -1LL);
	}
	if (strcmp(patched.key, key)) {
		free(patched.data);
		memset(&patched, 0, sizeof(patched));
		snprintf(patched.key, sizeof(patched.key), "%s", key);
		patched.result = PATCH_UNREADABLE;
		size_t romN = 0;
		uint8_t* rom = emu_rom_read_all(romPath, &romN);
		int applied = 0, failed = PATCH_NONE;
		if (rom) {
			patched.fileCrc = doCrc32(rom, romN);
			for (int i = 0; i < n; ++i) {
				size_t pn = 0;
				uint8_t* p = read_file(files[i], &pn);
				uint8_t* out = NULL;
				size_t outN = 0;
				int r = p ? patch_apply(p, pn, rom, romN, &out, &outN) : PATCH_UNREADABLE;
				free(p);
				fprintf(stderr, "patch %s: %s\n", patch_name(files[i]),
				        r == PATCH_APPLIED     ? "applied"
				        : r == PATCH_WRONG_ROM ? "made for another ROM, not applied"
				                               : "can't be read, not applied");
				if (r == PATCH_APPLIED) {
					free(rom);
					rom = out;
					romN = outN;
					size_t l = strlen(patched.names);
					snprintf(patched.names + l, sizeof(patched.names) - l, "%s%s", l ? " + " : "", patch_name(files[i]));
					++applied;
				} else {
					if (failed == PATCH_NONE) failed = r;
					size_t l = strlen(patched.skipped);
					snprintf(patched.skipped + l, sizeof(patched.skipped) - l, "%s%s", l ? " + " : "", patch_name(files[i]));
				}
			}
			patched.result = failed == PATCH_NONE ? PATCH_APPLIED : applied ? PATCH_PARTIAL : failed;
			if (!applied) snprintf(patched.names, sizeof(patched.names), "%s", patched.skipped);
			if (applied) {
				patched.data = rom;
				patched.size = romN;
				rom = NULL;
			}
		}
		free(rom);
	}
	*result = patched.result;
	if (fileCrc) *fileCrc = patched.fileCrc;
	if (patchName) snprintf(patchName, 256, "%s", patched.names);
	if (skipped) snprintf(skipped, 256, "%s", patched.skipped);
	uint8_t* copy = NULL;
	if (patched.data && (copy = malloc(patched.size))) {
		memcpy(copy, patched.data, patched.size);
		*size = patched.size;
	}
	pthread_mutex_unlock(&patchedLock);
	return copy;
}

static uint8_t* patched_rom(const char* romPath, size_t* size, int* result, char* patchName, uint32_t* fileCrc,
                            char* skipped) {
	static char files[PLAN_MAX][1024];
	static pthread_mutex_t filesLock = PTHREAD_MUTEX_INITIALIZER;
	int n = 0;
	bool planned = false;
	pthread_mutex_lock(&filesLock);
	pthread_mutex_lock(&patchedLock);
	if (plan.rom[0] && !strcmp(plan.rom, romPath)) {
		planned = true;
		for (int i = 0; i < plan.n; ++i) memcpy(files[n++], plan.files[i], sizeof(files[0]));
	}
	pthread_mutex_unlock(&patchedLock);
	if (!planned && patch_find(romPath, files[0], sizeof(files[0]))) n = 1;
	uint8_t* p = patched_with(romPath, files, n, size, result, patchName, fileCrc, skipped);
	pthread_mutex_unlock(&filesLock);
	return p;
}

uint8_t* emu_rom_read_patched(const char* path, char files[][1024], int n, size_t* size, int* patch, char* patchName) {
	int r;
	uint8_t* p = patched_with(path, files, n, size, &r, patchName, NULL, NULL);
	if (patch) *patch = r;
	return p ? p : emu_rom_read_all(path, size);
}

/* (a patched ROM is built in the heap, so a room given back after the code memory was placed under it stays out of reach;
 * a 32 MiB ROM's patch never fits and its read takes the room) */
static bool patches_apply(const char* romPath) {
	char beside[1024];
	bool planned = false, any = false;
	pthread_mutex_lock(&patchedLock);
	if (plan.rom[0] && !strcmp(plan.rom, romPath)) planned = true, any = plan.n > 0;
	pthread_mutex_unlock(&patchedLock);
	return planned ? any : patch_find(romPath, beside, sizeof(beside));
}

/* top: a plain ROM file into a top block (read_file_to), for a core's own cartridge only (freed with rom_free) */
static uint8_t* read_played(const char* path, size_t* size, int* patch, char* patchName, uint32_t* fileCrc, bool top) {
	int r;
	uint8_t* p = patched_rom(path, size, &r, patchName, fileCrc, NULL);
	if (patch) *patch = r;
	if (p) return p;
	uint8_t* rom = emu_is_archive_name(path) ? zip_first_rom(path, 0, size) : read_file_to(path, size, top);
	if (rom && fileCrc) *fileCrc = doCrc32(rom, *size);
	return rom;
}

uint8_t* emu_rom_read_played(const char* path, size_t* size, int* patch, char* patchName, uint32_t* fileCrc) {
	return read_played(path, size, patch, patchName, fileCrc, false);
}

/* ---------------------------------------------- one ROM, several cores
 * The game, its hidden copy and its logo's core run the same cartridge. On
 * the console its 32 MiB can't be had twice, so the cores opened on a ROM
 * that another has open (same file, same patches) read that one's buffer;
 * the buffer goes when the last of them closes. */
typedef struct SharedRom {
	char key[1100];
	uint8_t* rom;
	uint32_t romSize, romMask;
	uint32_t crc;
	int users;
	/* what emu_open tells of the ROM, for the cores that borrow it unread
	 * (the console: no second reading of the file) */
	char path[1024];
	int patch;
	char patchName[sizeof(((Emu*) 0)->patchName)];
	uint32_t fileCrc;
} SharedRom;
#define SHARED_MAX 4
static SharedRom sharedRoms[SHARED_MAX];
static pthread_mutex_t sharedLock = PTHREAD_MUTEX_INITIALIZER;

static void shared_key(const char* romPath, const char* patchName, char* key, size_t n) {
	struct stat st;
	bool ok = stat(romPath, &st) == 0;
	snprintf(key, n, "%s|%lld|%lld|%s", romPath, ok ? (long long) st.st_size : -1LL, ok ? (long long) st.st_mtime : -1LL,
	         patchName ? patchName : "");
}

static void split_release(ArmCore* c);
static void split_frame(ArmCore* c);
static bool hudFresh; /* (a new HUD picture since emu_hud_fresh last asked) */

/* ---------------------------------------------- battery save */

static size_t save_size(struct GBA* gba) {
	return gba->savedata.type == SAVEDATA_AUTODETECT ? 0 : savedataSize(gba);
}

static void save_read(ArmCore* c) {
	size_t n = 0;
	uint8_t* file = read_file(c->savePath, &n);
	if (!file) return;
	if (n > sizeof(c->gba->savedata.storage)) n = sizeof(c->gba->savedata.storage);
	memcpy(c->gba->savedata.storage, file, n);
	free(file);
}

/* gba-ds.cfg armgba_save: 0 a new file put in the old one's place, in the game's own frame; 1 the same on a thread of
 * its own (the SD card can keep the game waiting for seconds). The file is never written over in place: power lost
 * half-way would leave half old, half new. */
static int save_mode(void) {
	const char* v = getenv("GBADS_ARMGBA_SAVE");
	return v ? atoi(v) & 1 : 1;
}

static double ms_since(const struct timespec* t0) {
	struct timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0->tv_sec) * 1000.0 + (t1.tv_nsec - t0->tv_nsec) / 1e6;
}

static bool save_file(const char* path, const uint8_t* data, size_t n) {
	size_t len = strlen(path) + 8;
	char* tmp = malloc(len);
	if (!tmp) return false;
	snprintf(tmp, len, "%s.new", path);
	FILE* f = fopen(tmp, "wb");
	bool ok = f && fwrite(data, 1, n, f) == n;
	if (f) ok = fclose(f) == 0 && ok;
	if (!ok) {
		fprintf(stderr, "save: couldn't write %s\n", tmp);
		remove(tmp);
	} else {
		remove(path); /* (FAT: rename doesn't replace) */
		ok = rename(tmp, path) == 0;
		/* (the old file is gone: if only this failed, the new one is the only whole copy and stays) */
		if (!ok) fprintf(stderr, "save: couldn't put %s in place of %s\n", tmp, path);
	}
	free(tmp);
	return ok;
}

/* (the log line says what the SD card cost: the console's bench lost 0.2 to 14 s to it at each save. No float printing:
 * newlib keeps a thread's float buffers for good, and a thread for each game leaves them high in the heap, in the way of the
 * next big ROM) */
static bool save_timed(const char* path, const uint8_t* data, size_t n, unsigned frame) {
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	bool ok = save_file(path, data, n);
	fprintf(stderr, "save: %u bytes written at frame %u in %u ms\n", (unsigned) n, frame, (unsigned) (ms_since(&t0) + 0.5));
	return ok;
}

static bool save_write(ArmCore* c) {
	size_t n = save_size(c->gba);
	if (!c->savePath || !n) return false;
	return save_timed(c->savePath, c->gba->savedata.storage, n, c->gba->video.frameCounter);
}

/* The writing thread, made at the first save and kept until its game closes: a game's saves are
 * handed to it as copies. */
static struct {
	pthread_mutex_t lock;
	pthread_cond_t wake;
	pthread_t thread;
	bool started, busy, quit;
	char* path;
	uint8_t* data;
	size_t n;
	unsigned frame;
} saver = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };

static void* saver_main(void* arg) {
	(void) arg;
	pthread_mutex_lock(&saver.lock);
	for (;;) {
		while (!saver.busy && !saver.quit) pthread_cond_wait(&saver.wake, &saver.lock);
		if (!saver.busy) break;
		pthread_mutex_unlock(&saver.lock);
		save_timed(saver.path, saver.data, saver.n, saver.frame);
		pthread_mutex_lock(&saver.lock);
		free(saver.path);
		free(saver.data);
		saver.busy = false;
		pthread_cond_broadcast(&saver.wake);
	}
	pthread_mutex_unlock(&saver.lock);
	return NULL;
}

/* Waits for the write in hand, and ends the thread. */
static void saver_stop(void) {
	if (!saver.started) return;
	pthread_mutex_lock(&saver.lock);
	saver.quit = true;
	pthread_cond_broadcast(&saver.wake);
	pthread_mutex_unlock(&saver.lock);
	pthread_join(saver.thread, NULL);
	saver.started = saver.quit = false;
}

/* The thread, if there is room for it (a 32 MiB game leaves the heap little). */
static bool saver_start(void) {
	if (saver.started) return true;
#ifdef __3DS__
	if (host_heap_free() < (1536u << 10)) return false;
#endif
	saver.quit = false;
#ifdef __3DS__
	host_next_thread_name("battery save");
#endif
	saver.started = pthread_create(&saver.thread, NULL, saver_main, NULL) == 0;
	return saver.started;
}

/* A save handed to the thread; false: it's still on the last one (try again next frame). Without a thread
 * (none could be made, or no memory for the copy) the game writes it itself. */
static bool save_hand_off(ArmCore* c) {
	size_t n = save_size(c->gba);
	if (!c->savePath || !n) return true;
	pthread_mutex_lock(&saver.lock);
	bool busy = saver.busy;
	pthread_mutex_unlock(&saver.lock);
	if (busy) return false;
	char* path = strdup(c->savePath);
	uint8_t* data = malloc(n);
	if (!path || !data || !saver_start()) {
		free(path);
		free(data);
		save_write(c);
		return true;
	}
	memcpy(data, c->gba->savedata.storage, n);
	pthread_mutex_lock(&saver.lock);
	saver.path = path;
	saver.data = data;
	saver.n = n;
	saver.frame = c->gba->video.frameCounter;
	saver.busy = true;
	pthread_cond_broadcast(&saver.wake);
	pthread_mutex_unlock(&saver.lock);
	return true;
}

/* The game's writes to its save go to the file once they've stopped for a
 * quarter of a second (and as the game is closed). */
static void save_tick(ArmCore* c) {
	if (!c->savePath) return;
	if (c->gba->savedata.dirty) {
		c->gba->savedata.dirty = false;
		c->savePending = true;
		c->saveQuiet = 0;
	} else if (c->savePending && ++c->saveQuiet >= 15) {
		if (save_mode() & 1) {
			c->savePending = !save_hand_off(c);
		} else {
			c->savePending = false;
			save_write(c);
		}
	}
}

/* ---------------------------------------------- the warm list */

/* armGBA's list of the cartridge code the game ran compiled, kept between
 * runs (as armGBA-3DS keeps it): the next run compiles it in the frames'
 * spare time before the game gets there, not in a burst when it does (a race
 * in Mario Kart reached new code for minutes: 50-150 ms frames). */
#ifdef __3DS__
#define WARM_DIR "/3ds/gba-ds/warm"
#define WARM_MAGIC 0x4D524157u /* "WARM" */
#define WARM_MOST (1 << 16)

static void warm_load(ArmCore* c, uint32_t romCrc) {
	snprintf(c->warmPath, sizeof(c->warmPath), WARM_DIR "/%08lX.warm", (unsigned long) romCrc);
	FILE* f = fopen(c->warmPath, "rb");
	if (!f) return;
	uint32_t head[2];
	uint32_t* pairs = NULL;
	if (fread(head, 4, 2, f) == 2 && head[0] == WARM_MAGIC && head[1] <= WARM_MOST) {
		pairs = malloc((size_t) head[1] * 8 + 8);
		if (pairs && fread(pairs, 8, head[1], f) == head[1]) {
			jitWarmGive(c->gba, pairs, head[1]);
			fprintf(stderr, "warm list: %lu blocks from %s\n", (unsigned long) head[1], c->warmPath);
		}
	}
	free(pairs);
	fclose(f);
}

static void warm_save(ArmCore* c) {
	if (!c->warmPath[0] || !c->gba->jit) return;
	uint32_t* data = malloc(8 + (size_t) WARM_MOST * 8);
	if (!data) return;
	size_t n = jitWarmTake(c->gba, data + 2, WARM_MOST);
	data[0] = WARM_MAGIC;
	data[1] = (uint32_t) n;
	if (n) {
		mkdir(WARM_DIR, 0777);
		char tmp[80];
		snprintf(tmp, sizeof(tmp), "%s.new", c->warmPath);
		FILE* f = fopen(tmp, "wb");
		bool ok = f && fwrite(data, 1, 8 + n * 8, f) == 8 + n * 8;
		if (f) ok = fclose(f) == 0 && ok;
		if (ok) {
			remove(c->warmPath);
			ok = rename(tmp, c->warmPath) == 0;
		}
		if (!ok) remove(tmp);
		fprintf(stderr, "warm list: %lu blocks to %s%s\n", (unsigned long) n, c->warmPath, ok ? "" : " (couldn't write it)");
	}
	free(data);
}
#else
static void warm_save(ArmCore* c) { (void) c; }
#endif

/* ---------------------------------------------- open, close, frames */

/* gba-ds.cfg armgba_stats = N: every N frames of the game (of a copy: of those it ran) and as a machine closes, a log
 * line of what the recompiler did. The ms compiling are this machine's; those in cache calls are the whole program's. */
#ifdef __3DS__
extern uint64_t jitFlushTicks;
extern uint32_t jitFlushCalls;
extern bool jitStatsOn;
extern uint64_t renderWaitTicks, renderWorkTicks, audioWaitTicks, audioWorkTicks;
uint64_t renderThreadHostTicks(void);
static uint64_t pictureTicks, splitTicks;

struct JitSeen {
	uint64_t compiled, deferred, flushes0, flushes1, flushesAll, heapLow, freed, compileTicks, flushTicks, flushWork, invalidations, codeWrites, stepped, revived;
	uint32_t flushCalls, loads;
	uint64_t renderWait, renderWork, audioWait, audioWork, picture, split;
	unsigned last;
};

/* In a bench job the line goes with the job's results (the log's ring drops writes while the card is busy, and did drop these);
 * otherwise to the log. Its words don't start "threads:", which the bench's own line of where each thread ran does. */
static void stat_line(const char* format, ...) {
	char line[600];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (bench_active()) bench_note(line);
	else fprintf(stderr, "%s\n", line);
}

/* (the game's core, once its machine is made: the key is read here, so a bench job's options can switch it) */
static void jit_stats_open(ArmCore* c, bool game) {
	const char* v = getenv("GBADS_ARMGBA_STATS");
	int every = v ? atoi(v) : 0;
	if (game) jitStatsOn = every > 0;
	else if (!jitStatsOn) every = 0; /* (a copy counts when the game's machine did: opened after it) */
	c->seen = every > 0 ? calloc(1, sizeof(*c->seen)) : NULL;
	if (!c->seen) return;
	c->statsEvery = (unsigned) every;
	c->seen->flushTicks = jitFlushTicks; /* (these two are the whole program's, not this machine's) */
	c->seen->flushCalls = jitFlushCalls;
	c->seen->renderWait = renderWaitTicks;
	c->seen->renderWork = renderWorkTicks;
	c->seen->audioWait = audioWaitTicks;
	c->seen->audioWork = audioWorkTicks;
	c->seen->picture = pictureTicks;
	c->seen->split = splitTicks;
}

static void jit_stats(ArmCore* c, unsigned frame, bool game, bool closing) {
	struct JitSeen* seen = c->seen;
	struct Jit* jit = c->gba->jit;
	if (!seen || !jit || (!closing && frame % c->statsEvery)) return;
	double const perMs = 268111.856, frames = frame - seen->last ? frame - seen->last : 1;
	if (game) stat_line("jit threads: frames %u-%u: game thread waits for the renderer %.2f ms and the mixer %.2f ms a frame; renderer works %.2f ms, mixer %.2f ms; the game thread's picture %.2f ms, second picture %.2f ms",
	        seen->last, frame, (renderWaitTicks - seen->renderWait) / perMs / frames, (audioWaitTicks - seen->audioWait) / perMs / frames,
	        (renderWorkTicks - seen->renderWork) / perMs / frames, (audioWorkTicks - seen->audioWork) / perMs / frames, (pictureTicks - seen->picture) / perMs / frames,
	        (splitTicks - seen->split) / perMs / frames);
	uint64_t const others = jit->flushes - seen->flushesAll - (jit->flushReasons[0] - seen->flushes0) - (jit->flushReasons[1] - seen->flushes1);
	stat_line("jit%s%s: frames %u-%u: compiled +%u, first visits stepped +%u, flushes +%u by writes or waits +%u by full memory (%u of them heap low) +%u by states or resets, emptying %.1f ms (%u blocks freed), compiling %.2f ms and, for all machines, cache calls %.2f ms (%u) a frame",
	        game ? "" : " copy", closing ? " (closing)" : "", seen->last, frame, (unsigned) (jit->compiled - seen->compiled), (unsigned) (jit->deferred - seen->deferred),
	        (unsigned) (jit->flushReasons[0] - seen->flushes0), (unsigned) (jit->flushReasons[1] - seen->flushes1), (unsigned) (jit->heapLowFlushes - seen->heapLow),
	        (unsigned) others, (jit->flushWorkTicks - seen->flushWork) / perMs, (unsigned) (jit->flushedBlocks - seen->freed),
	        (jit->compileTicks - seen->compileTicks) / perMs / frames, (jitFlushTicks - seen->flushTicks) / perMs / frames, (unsigned) (jitFlushCalls - seen->flushCalls));
	stat_line("jit%s code: frames %u-%u: code %u of %u KiB (hot %u, cold %u, careful copies %u, unused %u), heap %u KiB free; invalidated +%u, writes to code +%u, instructions stepped +%u, revived +%u, states loaded +%u; compiles refused so far: heap low %u, no allocation %u, no code room %u",
	        game ? "" : " copy", seen->last, frame, (unsigned) ((jit->codeNext - jit->codeStart) >> 10),
	        (unsigned) (jit->codeSize >> 10), (unsigned) (jit->hotBytes >> 10), (unsigned) (jit->coldBytes >> 10),
	        (unsigned) (jit->carefulBytes >> 10), (unsigned) (jit->wastedBytes >> 10), (unsigned) (host_heap_free() >> 10),
	        (unsigned) (jit->invalidations - seen->invalidations), (unsigned) (jit->codeWrites - seen->codeWrites),
	        (unsigned) (jit->stepped - seen->stepped), (unsigned) (jit->revived - seen->revived), (unsigned) (c->loads - seen->loads),
	        (unsigned) jit->refused[0], (unsigned) jit->refused[1], (unsigned) jit->refused[2]);
	seen->loads = c->loads;
	seen->invalidations = jit->invalidations;
	seen->codeWrites = jit->codeWrites;
	seen->stepped = jit->stepped;
	seen->revived = jit->revived;
	seen->compiled = jit->compiled;
	seen->deferred = jit->deferred;
	seen->flushes0 = jit->flushReasons[0];
	seen->flushes1 = jit->flushReasons[1];
	seen->flushesAll = jit->flushes;
	seen->heapLow = jit->heapLowFlushes;
	seen->freed = jit->flushedBlocks;
	seen->flushWork = jit->flushWorkTicks;
	seen->compileTicks = jit->compileTicks;
	seen->flushTicks = jitFlushTicks;
	seen->flushCalls = jitFlushCalls;
	seen->renderWait = renderWaitTicks;
	seen->renderWork = renderWorkTicks;
	seen->audioWait = audioWaitTicks;
	seen->audioWork = audioWorkTicks;
	seen->picture = pictureTicks;
	seen->split = splitTicks;
	seen->last = frame;
}
#endif

#ifdef __3DS__
static void twin_open(ArmCore* c, bool oneWorker);
static void twin_close(ArmCore* c);
static bool raw_open(ArmCore* c);
#endif
static void wide_free(ArmCore* c);
static void close_core(ArmCore* c) {
	if (!c) return;
#ifdef __3DS__
	if (c->gba && c->seen) jit_stats(c, c->game ? c->gba->video.frameCounter : c->ran, c->game, true);
	if (c->game) host_jit_game_rom(0);
#endif
	if (c->gba) {
		warm_save(c);
		if (c->savePath) saver_stop(); /* (what it has in hand lands before the last write) */
		if (c->savePath && (c->savePending || c->gba->savedata.dirty)) save_write(c);
		if (c->cheatsOn) cheatsClear(&c->cheats, c->gba);
		if (c->shared) {
			pthread_mutex_lock(&sharedLock);
			c->gba->rom = NULL; /* gbaDestroy frees what's there */
			if (--c->shared->users == 0) {
				rom_free(c->shared->rom);
				memset(c->shared, 0, sizeof(*c->shared));
			}
			pthread_mutex_unlock(&sharedLock);
		}
#ifdef __3DS__
		else if (host_top_owns(c->gba->rom)) {
			host_top_free(c->gba->rom); /* (not malloc's, so not gbaDestroy's to free) */
			c->gba->rom = NULL;
		}
#endif
		/* (its render thread first: it draws with the hooks) */
		renderThreadStop(c->gba);
		split_release(c);
		renderSetHooks(c->gba, NULL);
		gbaDestroy(c->gba);
	}
	free(c->markFrame);
#ifdef __3DS__
	twin_close(c);
#endif
	wide_free(c);
	free(c->savePath);
#ifdef __3DS__
	free(c->seen);
#endif
	free(c->ring);
	free(c);
}

static uint32_t colour_8888(uint16_t v);
static void widen_tables(void);
static void widen_row(uint32_t* dst, const uint16_t* src);
/* Each wide buffer's bright samples (emu_bright) row by row, counted by the worker that widens the row while its source
 * is in that worker's cache; 0xFF: a row not widened yet (set only at open: every frame widens all 160 rows, so none goes stale). */
static const uint32_t* brightBase[2];
static uint8_t brightRows[2][GBA_H];

int emuRenderWorkers = 1;
bool emuHiddenCopy;

/* What src/emu.c has for libmgba's Game Boy core, the cartridge's motor and the BIOS's states (GBA DS 1.6): armGBA
 * here runs GBA games only (emu_is_rom_name takes no .gb), drives no motor, and its states don't depend on which
 * BIOS made them. */
bool emu_is_gb(const Emu* e) {
	(void) e;
	return false;
}
int emu_gb_kind(const Emu* e) {
	(void) e;
	return 0;
}
void emu_gb_colours(int colors, const uint32_t* rgb, bool mono) {
	(void) colors, (void) rgb, (void) mono;
}
float emu_rumble_level(const Emu* e) {
	(void) e;
	return 0.0f;
}
BiosRefusal emu_state_file_bios_refusal(Emu* e, const char* path) {
	(void) e, (void) path;
	return BIOS_REFUSAL_NONE;
}

/* The game's code and title from its header, as src/emu.c has them for a GBA game. */
bool emu_rom_code(const uint8_t* h, size_t n, char code[5]) {
	if (n < 0xB0) return false;
	memcpy(code, h + 0xAC, 4);
	code[4] = 0;
	return true;
}
void emu_rom_title(const uint8_t* h, size_t n, char title[17]) {
	title[0] = 0;
	if (n < 0xA0 + 12) return;
	memcpy(title, h + 0xA0, 12);
	title[12] = 0;
	for (int i = 0; i < 12; ++i) {
		if (title[i] && (title[i] < 0x20 || title[i] > 0x7E)) title[i] = '_';
	}
}

/* The sun sensor's reading (Boktai), as src/emu.c gives mGBA: the setting's level (0-10) in mGBA's own steps,
 * lower the brighter. Set before each frame. */
int emu_solar_level = 6;
static uint8_t solar_reading(void) {
	static const uint8_t levels[10] = { 5, 11, 18, 27, 42, 62, 84, 109, 139, 183 };
	int level = emu_solar_level < 0 ? 0 : emu_solar_level > 10 ? 10 : emu_solar_level;
	return (uint8_t) (0xFF - (0x16 + (level > 0 ? levels[level - 1] : 0)));
}

/* The player's own GBA BIOS (Settings > System): its 16 KiB, read once and kept (each machine maps it). */
char emu_bios_path[512];
BiosFile emu_bios_check(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return BIOS_FILE_MISSING;
	static uint8_t probe[SIZE_BIOS + 1];
	size_t n = fread(probe, 1, sizeof(probe), f);
	fclose(f);
	if (n != SIZE_BIOS) return BIOS_FILE_SIZE;
	/* (its reset vector: a branch, as every GBA BIOS starts) */
	return probe[3] == 0xEA ? BIOS_FILE_OK : BIOS_FILE_NOT_BIOS;
}
static const uint8_t* own_bios(void) {
	static uint8_t file[SIZE_BIOS];
	static char loaded[512];
	if (!emu_bios_path[0]) return NULL;
	if (!strcmp(loaded, emu_bios_path)) return file;
	if (emu_bios_check(emu_bios_path) != BIOS_FILE_OK) return NULL;
	FILE* f = fopen(emu_bios_path, "rb");
	bool ok = f && fread(file, 1, sizeof(file), f) == sizeof(file);
	if (f) fclose(f);
	loaded[0] = 0;
	if (!ok) return NULL;
	snprintf(loaded, sizeof(loaded), "%s", emu_bios_path);
	return file;
}

/* (testing, as emu.c's: GBADS_RTC_FIXED=<unix seconds> holds the cartridge clock still; the bench sets it) */
static int64_t rtcFixed;
static int64_t rtc_fixed_now(void* context) {
	(void) context;
	return rtcFixed;
}

bool emu_open(Emu* e, const char* romPath, const char* savePath, bool audio) {
	memset(e, 0, sizeof(*e));
	e->fbPitch = GBA_W;
	ArmCore* c = calloc(1, sizeof(*c));
	if (!c) return false;
	e->core = (struct mCore*) c;
	c->layers = 0x1F;
	c->bright = -1;
#ifdef __3DS__
	/* (the game's own core is the one with sound: the large share of the
	 * recompiler's memory, and the console's other cores to draw and mix on) */
	pthread_mutex_lock(&sharedLock);
	/* (the ROM's buffer, set aside before the recompiler's code memory and
	 * the machine are made, and given back just before the ROM is read into
	 * it: after a while in the game list (the logos' copies coming and going)
	 * the heap had 30 MiB free but not 8 in one piece once the game's code
	 * memory was placed, and Pokémon Pinball didn't start) */
	void* romRoom = NULL;
	size_t romRoomBytes = 0;
	{
		/* (the ROM is read after the machine is made: its size, so that the
		 * recompiler leaves it room; a .zip's isn't known, taken as 16 MiB) */
		size_t toLoad = 0;
		bool held = false;
		for (int i = 0; i < SHARED_MAX; ++i) held |= sharedRoms[i].users && !strcmp(sharedRoms[i].path, romPath);
		struct stat rst;
		if (!held) toLoad = emu_is_archive_name(romPath) || stat(romPath, &rst) ? 16u << 20 : (size_t) rst.st_size;
		/* (a plain file's room is at the top of the heap's range: the code memory placed first can push the break up past it) */
		romRoomBytes = gbaRomBufferSize(toLoad);
		if (toLoad && emu_is_archive_name(romPath)) romRoom = malloc(romRoomBytes);
		else if (toLoad && (toLoad > (16u << 20) || !patches_apply(romPath))) romRoom = host_top_alloc(romRoomBytes);
		/* (set aside already: the heap free doesn't count it, so nothing more
		 * is kept for it) */
		host_jit_main(audio, romRoom ? 0 : toLoad, emuHiddenCopy);
	}
	c->gba = gbaCreate();
	pthread_mutex_unlock(&sharedLock);
#else
	c->gba = gbaCreate();
#endif
	if (!c->gba) {
		fprintf(stderr, "armGBA: no memory for a core\n");
		goto fail;
	}
	struct GBA* gba = c->gba;
	threadLogsLean = !getenv("GBADS_ARMGBA_LEAN_LOGS") || atoi(getenv("GBADS_ARMGBA_LEAN_LOGS")); /* (armgba_lean_logs = 0: a barrier at every entry of the sound log) */
	c->compileBudget = getenv("GBADS_ARMGBA_COMPILE_BUDGET") ? (uint64_t) (atof(getenv("GBADS_ARMGBA_COMPILE_BUDGET")) * 268111.856) : 268111 * 3 / 2; /* (armgba_compile_budget, in ms: 1.5 when unset) */
	jitSetCompileAfter(getenv("GBADS_ARMGBA_COMPILE_AFTER") ? atoi(getenv("GBADS_ARMGBA_COMPILE_AFTER")) : 0); /* (armgba_compile_after: 0 when unset, so a bench job after another gets the built 2 back) */
	if (getenv("GBADS_ARMGBA_MID_AFTER")) jitSetMidAfter(gba, atoi(getenv("GBADS_ARMGBA_MID_AFTER"))); /* (armgba_mid_after: 0 steps every return into a block) */
	if (getenv("GBADS_ARMGBA_DEFER")) jitSetDeferMore(gba, atoi(getenv("GBADS_ARMGBA_DEFER")) != 0); /* (armgba_defer = 0: only events that start no DMA run after the instruction they fall in) */
	if (getenv("GBADS_ARMGBA_LAZY_CARRY")) jitSetLazyCarry(gba, atoi(getenv("GBADS_ARMGBA_LAZY_CARRY")) != 0); /* (armgba_lazy_carry = 1: a console A/B) */
	if (getenv("GBADS_ARMGBA_MID_CAREFUL")) jitSetMidCareful(gba, atoi(getenv("GBADS_ARMGBA_MID_CAREFUL"))); /* (armgba_mid_careful = 1, 2 or 3: a console A/B) */
	gbaCside = getenv("GBADS_ARMGBA_CSIDE") ? (uint32_t) atoi(getenv("GBADS_ARMGBA_CSIDE")) : 0; /* (armgba_cside: a bit mask of exact C-side savings, 0 when unset; a console A/B) */
	c->hooks.context = c;
	renderSetHooks(gba, &c->hooks);
	gbaLoadBios(gba, armgbaBios, SIZE_BIOS);
	if (own_bios()) gbaLoadBios(gba, own_bios(), SIZE_BIOS);
#ifndef __3DS__
	{
		/* GBADS_BIOS=<a GBA BIOS> (tests: the same BIOS on libmgba and armGBA, 3ds/test/dualrun.py) */
		static uint8_t file[SIZE_BIOS];
		FILE* bf = getenv("GBADS_BIOS") ? fopen(getenv("GBADS_BIOS"), "rb") : NULL;
		if (bf && fread(file, 1, sizeof(file), bf) == sizeof(file)) gbaLoadBios(gba, file, SIZE_BIOS);
		else if (getenv("GBADS_BIOS")) fprintf(stderr, "GBADS_BIOS: can't load %s\n", getenv("GBADS_BIOS"));
		if (bf) fclose(bf);
	}
#endif

	size_t size = 0;
	uint8_t* rom = NULL;
	bool ok = false;
	SharedRom* s = NULL;
	SharedRom* spare = NULL;
#ifdef __3DS__
	/* A core opened on the file another has open takes that one's cartridge
	 * as it is: neither the file read again nor 32 MiB more, which aren't
	 * there to be had. */
	pthread_mutex_lock(&sharedLock);
	for (int i = 0; i < SHARED_MAX; ++i) {
		if (sharedRoms[i].users && !strcmp(sharedRoms[i].path, romPath)) s = &sharedRoms[i];
	}
	if (s) {
		gbaOfferRom(s->rom);
		ok = gbaLoadRom(gba, s->rom, s->romSize);
		if (gbaOfferedRom()) {
			gbaOfferRom(NULL);
			ok = false;
		}
		if (ok) {
			++s->users;
			c->shared = s;
			e->patch = s->patch;
			memcpy(e->patchName, s->patchName, sizeof(e->patchName));
			e->fileCrc = s->fileCrc;
			e->romCrc = s->crc;
			memcpy(e->gameCode, s->romSize >= 0xB0 ? (const char*) s->rom + 0xAC : "\0\0\0\0", 4);
			e->gameCode[4] = 0;
		}
	}
	pthread_mutex_unlock(&sharedLock);
	if (s && !ok) {
		fprintf(stderr, "could not borrow the ROM of %s\n", romPath);
		goto fail;
	}
#endif
	if (!s) {
#ifdef __3DS__
		/* (a plain file's room goes to its read as it is; a .zip's is given back first) */
		if (romRoom && host_top_owns(romRoom)) romRoomNow = romRoom, romRoomSize = romRoomBytes;
		else host_rom_free(romRoom);
		romRoom = NULL;
#endif
		rom = read_played(romPath, &size, &e->patch, e->patchName, &e->fileCrc, true);
#ifdef __3DS__
		host_rom_free(romRoomNow), romRoomNow = NULL;
#endif
		{
			/* (the names of the ones left out, as src/emu.c reports them) */
			int r;
			size_t n2 = 0;
			char names[256];
			uint8_t* again = e->patch == PATCH_PARTIAL ? patched_rom(romPath, &n2, &r, names, NULL, e->patchSkipped) : NULL;
			free(again);
		}
		if (!rom) {
			fprintf(stderr, "could not load ROM %s\n", romPath);
			goto fail;
		}
		e->romCrc = doCrc32(rom, size);
		if (e->patch != PATCH_APPLIED && e->patch != PATCH_PARTIAL) e->fileCrc = e->romCrc;
		memcpy(e->gameCode, size >= 0xB0 ? (const char*) rom + 0xAC : "\0\0\0\0", 4);
		e->gameCode[4] = 0;

		/* The first core on this ROM loads it; the others read its buffer. */
		char key[1100];
		shared_key(romPath, e->patchName, key, sizeof(key));
		pthread_mutex_lock(&sharedLock);
		for (int i = 0; i < SHARED_MAX; ++i) {
			if (sharedRoms[i].users && !strcmp(sharedRoms[i].key, key) && sharedRoms[i].crc == e->romCrc) s = &sharedRoms[i];
			else if (!sharedRoms[i].users && !spare) spare = &sharedRoms[i];
		}
		if (s) {
			/* (what gbaLoadRom works out from the cartridge besides copying it) */
			ok = gbaLoadRom(gba, rom, size);
			if (ok) {
				free(gba->rom);
				gba->rom = s->rom;
				gba->romSize = s->romSize;
				gba->romMask = s->romMask;
				++s->users;
				c->shared = s;
			}
		} else {
#ifdef __3DS__
			/* the buffer the file was read into becomes the cartridge */
			size_t whole = gbaRomBufferSize(size);
			uint8_t* grown = whole > size && !host_top_owns(rom) ? realloc(rom, whole) : rom;
			if (grown) {
				rom = grown;
				if (whole > size) memset(rom + size, 0, whole - size);
				gbaOfferRom(rom);
			}
#endif
			ok = gbaLoadRom(gba, rom, size);
#ifdef __3DS__
			if (gbaOfferedRom()) gbaOfferRom(NULL);
			else if (ok) rom = NULL; /* taken */
#endif
			if (ok && spare && gba->rom) {
				snprintf(spare->key, sizeof(spare->key), "%s", key);
				spare->rom = gba->rom;
				spare->romSize = gba->romSize;
				spare->romMask = gba->romMask;
				spare->crc = e->romCrc;
				spare->users = 1;
				snprintf(spare->path, sizeof(spare->path), "%s", romPath);
				spare->patch = e->patch;
				memcpy(spare->patchName, e->patchName, sizeof(spare->patchName));
				spare->fileCrc = e->fileCrc;
				c->shared = spare;
			}
		}
		pthread_mutex_unlock(&sharedLock);
		rom_free(rom);
		if (!ok) {
			fprintf(stderr, "could not load ROM %s\n", romPath);
			goto fail;
		}
	}

#ifdef __3DS__
	host_rom_free(romRoom), romRoom = NULL; /* (a cartridge borrowed from another core: not needed) */
#endif
	gba->savedata.backed = true;
	if (savePath) {
		c->savePath = strdup(savePath);
		if (c->savePath) save_read(c);
		if (c->savePath && (save_mode() & 1)) saver_start(); /* (its log lines here, not in a frame) */
	}
#ifdef __3DS__
	c->codeCore = host_core_now();
#endif
	if (getenv("GBADS_RTC_FIXED")) {
		rtcFixed = atoll(getenv("GBADS_RTC_FIXED"));
		gba->gpio.clock = rtc_fixed_now;
	}
	gbaReset(gba, true);
	gba->savedata.dirty = false;
#ifdef __3DS__
	if (audio) {
		/* (render_workers = 2: a second thread draws every other line, on the
		 * fourth core beside the hidden copy. Late: each frame is shown one
		 * frame after it's made, so the game never waits for the drawing; the
		 * HUD picture, drawn one frame in hooks.secondEvery (6), with it) */
		if (renderThreadStart(gba, true, emuRenderWorkers == 2)) renderSetHooks(gba, &c->hooks), c->late = true;
		audioThreadStart(gba);
		warm_load(c, e->romCrc);
	}
#else
	/* (GBADS_RENDER_THREAD=1, tests: drawn on a thread as the console's game is;
	 * 2: on two, each drawing every other line) */
	/* (GBADS_RENDER_LATE=1: the game itself a frame late, as on the 3DS; its
	 * hidden copies aren't) */
	if (getenv("GBADS_RENDER_THREAD") && renderThreadStart(gba, audio && getenv("GBADS_RENDER_LATE") != NULL, atoi(getenv("GBADS_RENDER_THREAD")) == 2))
		renderSetHooks(gba, &c->hooks), c->late = audio && getenv("GBADS_RENDER_LATE") != NULL;
	if (getenv("GBADS_AUDIO_THREAD")) audioThreadStart(gba);
#endif

	e->fb = c->fbOwn = calloc(GBA_W * GBA_H, sizeof(uint32_t));
	e->audio = audio;
	if (c->late && getenv("GBADS_ARMGBA_PICTURE_THREAD") && atoi(getenv("GBADS_ARMGBA_PICTURE_THREAD")) == 1) {
#ifdef __3DS__
		c->wideLinear = emuRenderWorkers == 1 && raw_open(c);
#endif
		if (!c->wideLinear) {
			c->wideBuf[0] = calloc(GBA_W * GBA_H, sizeof(uint32_t));
			c->wideBuf[1] = calloc(GBA_W * GBA_H, sizeof(uint32_t));
		}
		if (c->wideBuf[0] && c->wideBuf[1]) {
			widen_tables();
#ifdef __3DS__
			if (!c->wideLinear) twin_open(c, emuRenderWorkers == 1);
#endif
			brightBase[0] = c->wideBuf[0], brightBase[1] = c->wideBuf[1];
			memset(brightRows, 0xFF, sizeof(brightRows));
			renderThreadSetWiden(gba, widen_row, c->wideBuf[0], c->wideBuf[1]);
		} else {
			wide_free(c);
		}
	}
#ifdef __3DS__
	c->game = audio;
	if (audio) host_jit_game_rom(gba->romSize);
	jit_stats_open(c, audio);
#endif
	if (audio) {
		c->ringCap = 8192; /* (a power of two: the ring's indexes are masked, the ARM11 has no divide) */
		c->ring = calloc(c->ringCap, 2 * sizeof(int16_t));
	}
	cheatsInit(&c->cheats);
	widen_tables(); /* (before any other thread takes a picture) */
	if (!e->fb || (audio && !c->ring)) goto fail;
#ifdef __3DS__
	host_mem_note(audio ? "the game opened" : "a copy opened");
#endif
	return true;
fail:
#ifdef __3DS__
	host_rom_free(romRoom);
#endif
	close_core(c);
	e->core = NULL;
	free(e->fb);
	e->fb = NULL;
	return false;
}

static Emu* rewindOwner;
void emu_close(Emu* e) {
#ifdef __3DS__
	if (e->fbCompactCopies) {
		char line[120];
		snprintf(line, sizeof(line), "the picture was copied to rows of 240 words %u times in %llu frames (raw_picture)", e->fbCompactCopies, (unsigned long long) e->frames);
		logLine("%s", line);
		if (bench_active()) bench_note(line);
	}
#endif
	if (e->core) {
		if (AC(e)->fbOwn) e->fb = AC(e)->fbOwn;
		e->fbPitch = GBA_W;
		if (rewindOwner == e) emu_rewind_deinit();
		close_core(AC(e));
		e->core = NULL;
	}
	free(e->fb);
	e->fb = NULL;
	e->fbGpu = NULL;
	free(e->fbCompact);
	e->fbCompact = NULL;
	e->fbCompactOk = false;
	e->fbPitch = GBA_W;
}

bool emu_cancelled(const Emu* e) {
	return e->cancel && *e->cancel;
}

/* Each bit of a color widens on its own, so a color is its low byte's entry ORed with its high byte's. */
static uint32_t widenLow[256], widenHigh[256];
static bool widenReady;
static void widen_tables(void) {
	if (__atomic_load_n(&widenReady, __ATOMIC_ACQUIRE)) return;
	for (uint32_t i = 0; i < 256; ++i) {
		widenLow[i] = colour_8888((uint16_t) i);
		widenHigh[i] = colour_8888((uint16_t) (i << 8));
	}
	__atomic_store_n(&widenReady, true, __ATOMIC_RELEASE);
}

static inline uint32_t widen(uint32_t v) {
	return widenLow[v & 0xFF] | widenHigh[(v >> 8) & 0xFF];
}

/* (the tables come in as pointers so each lookup is one load; the compiler would add their offsets into the addresses otherwise) */
static __attribute__((noipa)) void widen_words(uint32_t* restrict dst, const uint32_t* restrict words, const uint32_t* restrict low, const uint32_t* restrict high) {
	for (int i = 0; i < GBA_W * GBA_H / 2; i += 2) {
		uint32_t const a = words[i], b = words[i + 1];
		dst[2 * i] = low[a & 0xFF] | high[(a >> 8) & 0xFF];
		dst[2 * i + 1] = low[(a >> 16) & 0xFF] | high[a >> 24];
		dst[2 * i + 2] = low[b & 0xFF] | high[(b >> 8) & 0xFF];
		dst[2 * i + 3] = low[(b >> 16) & 0xFF] | high[b >> 24];
	}
}

#ifdef __3DS__
/* gpu_twin: the picture the workers widen is also written in the GPU's byte order (the words swapped, as the game thread's swap made them), into a
 * linear buffer that the worker flushes itself, when the last row is in. */
static struct {
	uint32_t* wide;
	uint32_t* twin;
} twinMap[2];

/* The whole twin is written back in one call when its last row is in (or, if that row never came, when the rows start on the
 * other twin); only the one worker calls this (twin_open: one worker). */
static const uint32_t* twinDirty;
static uint64_t twinRowTicks, twinFlushTicks;
static unsigned twinFrames;

/* Where row `wideRow` of a wide buffer has its twin (NULL: none), and which twin that is, for twin_row_done. */
static uint32_t* twin_row_at(const uint32_t* wideRow, int* y, uint32_t** twin) {
	for (int k = 0; k < 2; ++k) {
		const uint32_t* base = twinMap[k].wide;
		if (!base || wideRow < base || wideRow >= base + GBA_W * GBA_H) continue;
		*y = (int) ((wideRow - base) / GBA_W);
		*twin = twinMap[k].twin;
		return *twin + (size_t) *y * HOST_TWIN_STRIDE;
	}
	return NULL;
}

/* The row is in: the whole twin is written back in one call with its last row (or, if that row never came, when the rows
 * start on the other twin). Only the one worker calls this (twin_open: one worker). */
static void twin_row_done(int y, uint32_t* twin) {
	const size_t bytes = (size_t) GBA_H * HOST_TWIN_STRIDE * sizeof(uint32_t);
	if (twinDirty && twinDirty != twin) host_flush_data(twinDirty, bytes);
	if (y == GBA_H - 1) {
		const uint64_t t0 = host_ticks();
		host_flush_data(twin, bytes);
		twinFlushTicks += host_ticks() - t0;
		twinDirty = NULL;
		++twinFrames;
	} else {
		twinDirty = twin;
	}
}

static void twin_open(ArmCore* c, bool oneWorker) {
	c->twin[0] = c->twin[1] = NULL;
	if (!oneWorker || !host_gpu_twin_ready()) return;
	const size_t bytes = (size_t) GBA_H * HOST_TWIN_STRIDE * sizeof(uint32_t);
	for (int k = 0; k < 2; ++k) {
		c->twin[k] = host_linear_alloc(bytes);
		if (!c->twin[k]) break;
		memset(c->twin[k], 0, bytes);
		host_flush_data(c->twin[k], bytes);
	}
	if (!c->twin[0] || !c->twin[1]) {
		for (int k = 0; k < 2; ++k) {
			if (c->twin[k]) host_linear_free(c->twin[k]);
			c->twin[k] = NULL;
		}
		return;
	}
	twinDirty = NULL;
	for (int k = 0; k < 2; ++k) twinMap[k].wide = c->wideBuf[k], twinMap[k].twin = c->twin[k];
	logLine("picture twin: made (2 x %u KiB of linear memory)", (unsigned) (bytes >> 10));
}

static void twin_close(ArmCore* c) {
	if (!c->twin[0]) return;
	host_picture_twin(NULL, NULL); /* (a later game's picture may sit at the freed picture's address) */
	host_gpu_settle();
	if (twinFrames) {
		logLine("picture twin: per frame the render worker spent %.3f ms widening the rows with it and %.3f ms flushing it (%u frames)", twinRowTicks / HOST_TICKS_PER_MS / twinFrames,
		        twinFlushTicks / HOST_TICKS_PER_MS / twinFrames, twinFrames);
	}
	twinRowTicks = twinFlushTicks = 0, twinFrames = 0;
	twinDirty = NULL;
	for (int k = 0; k < 2; ++k) {
		twinMap[k].wide = twinMap[k].twin = NULL;
		host_linear_free(c->twin[k]);
		c->twin[k] = NULL;
	}
}

/* raw_picture: wideBuf is what the copy engine reads (with some slack after it), written back by the one
 * worker when its last row is in, as the twin's. */
static const uint32_t* rawBase[2];
static const uint32_t* rawDirty;
static uint64_t rawFlushTicks;
static unsigned rawFrames;

static unsigned rawPitch;

/* The buffer that row dst (as armGBA counts them: GBA_W words apart from the buffer's start) belongs to, and which row it is. */
static uint32_t* raw_row_at(const uint32_t* dst, int* y, const uint32_t** base) {
	*base = dst >= rawBase[1] && dst < rawBase[1] + GBA_W * GBA_H ? rawBase[1] : rawBase[0];
	*y = (int) ((dst - *base) / GBA_W);
	return (uint32_t*) *base + (size_t) *y * rawPitch;
}

static void raw_row_done(const uint32_t* base, int y) {
	const size_t bytes = (size_t) rawPitch * GBA_H * sizeof(uint32_t);
	if (rawDirty && rawDirty != base) host_flush_data(rawDirty, bytes);
	if (y == GBA_H - 1) {
		const uint64_t t0 = host_ticks();
		host_flush_data(base, bytes);
		rawFlushTicks += host_ticks() - t0;
		rawDirty = NULL;
		++rawFrames;
	} else {
		rawDirty = base;
	}
}

static bool raw_open(ArmCore* c) {
	if (!host_raw_picture_ready()) return false;
	c->widePitch = host_raw_picture_pitch();
	const size_t bytes = ((size_t) c->widePitch * GBA_H + HOST_RAW_SLACK_WORDS) * sizeof(uint32_t);
	for (int k = 0; k < 2; ++k) {
		c->wideBuf[k] = host_linear_alloc(bytes);
		if (!c->wideBuf[k]) break;
		memset(c->wideBuf[k], 0, bytes);
		host_flush_data(c->wideBuf[k], bytes);
	}
	if (!c->wideBuf[0] || !c->wideBuf[1]) {
		for (int k = 0; k < 2; ++k) {
			if (c->wideBuf[k]) host_linear_free(c->wideBuf[k]);
			c->wideBuf[k] = NULL;
		}
		logLine("raw picture: no linear memory for the picture buffers, the byte swap stays");
		return false;
	}
	rawDirty = NULL;
	rawPitch = c->widePitch;
	for (int k = 0; k < 2; ++k) rawBase[k] = c->wideBuf[k];
	logLine("raw picture: made (2 x %u KiB of linear memory, rows %u words apart)", (unsigned) (bytes >> 10), c->widePitch);
	return true;
}
#endif

/* (the copy engine may still be reading a raw picture: the GPU is waited for before it goes) */
static void wide_free(ArmCore* c) {
	if (c->wideBuf[0] && brightBase[0] == c->wideBuf[0]) brightBase[0] = brightBase[1] = NULL;
#ifdef __3DS__
	if (c->wideLinear) {
		host_picture_raw(NULL); /* (a later game's picture may sit at the freed picture's address) */
		host_gpu_settle();
		if (rawFrames) {
			char line[120];
			snprintf(line, sizeof(line), "raw picture: per frame the render worker spent %.3f ms flushing it (%u frames)", rawFlushTicks / HOST_TICKS_PER_MS / rawFrames, rawFrames);
			logLine("%s", line);
			if (bench_active()) bench_note(line);
		}
		rawFlushTicks = 0, rawFrames = 0;
		rawDirty = NULL;
		rawBase[0] = rawBase[1] = NULL;
		host_linear_free(c->wideBuf[0]);
		host_linear_free(c->wideBuf[1]);
		c->wideBuf[0] = c->wideBuf[1] = NULL;
		c->wideLinear = false;
		rawPitch = 0;
		return;
	}
#endif
	free(c->wideBuf[0]);
	free(c->wideBuf[1]);
	c->wideBuf[0] = c->wideBuf[1] = NULL;
}

static void bright_count(const uint32_t* dst, const uint16_t* src) {
	if (!brightBase[0]) return;
	int const k = dst >= brightBase[1] && dst < brightBase[1] + GBA_W * GBA_H;
	if (dst < brightBase[k] || dst >= brightBase[k] + GBA_W * GBA_H) return;
	int const y = (int) ((dst - brightBase[k]) / GBA_W);
	int n = 0;
	for (int x = (EMU_DARK_STEP - y * GBA_W % EMU_DARK_STEP) % EMU_DARK_STEP; x < GBA_W; x += EMU_DARK_STEP) n += emu_bright(widen(src[x]));
	brightRows[k][y] = (uint8_t) n;
}

static int bright_sum(int k) {
	int n = 0;
	for (int y = 0; y < GBA_H; ++y) {
		if (brightRows[k][y] == 0xFF) return -1;
		n += brightRows[k][y];
	}
	return n;
}

int emu_bright_samples(Emu* e) {
	ArmCore* const c = AC(e);
	if (!c) return -1;
	int const n = e->fbPainted ? -1 : c->bright;
	c->bright = -1;
	return n;
}

/* One row, from a render worker (its source is word aligned: the log's frame). */
static void widen_row(uint32_t* dst, const uint16_t* src) {
	const uint32_t* words = __builtin_assume_aligned(src, 4);
	bright_count(dst, src);
#ifdef __3DS__
	int y = 0;
	uint32_t* twin = NULL;
	uint32_t* const tw = twinMap[0].twin ? twin_row_at(dst, &y, &twin) : NULL;
	if (tw) {
		/* (the same words, swapped into the twin as they are made) */
		const uint64_t t0 = host_ticks();
		for (int i = 0; i < GBA_W / 2; i += 2) {
			uint32_t const a = words[i], b = words[i + 1];
			uint32_t const w0 = widenLow[a & 0xFF] | widenHigh[(a >> 8) & 0xFF], w1 = widenLow[(a >> 16) & 0xFF] | widenHigh[a >> 24];
			uint32_t const w2 = widenLow[b & 0xFF] | widenHigh[(b >> 8) & 0xFF], w3 = widenLow[(b >> 16) & 0xFF] | widenHigh[b >> 24];
			dst[2 * i] = w0, dst[2 * i + 1] = w1, dst[2 * i + 2] = w2, dst[2 * i + 3] = w3;
			tw[2 * i] = __builtin_bswap32(w0), tw[2 * i + 1] = __builtin_bswap32(w1);
			tw[2 * i + 2] = __builtin_bswap32(w2), tw[2 * i + 3] = __builtin_bswap32(w3);
		}
		twinRowTicks += host_ticks() - t0;
		twin_row_done(y, twin);
		return;
	}
	const uint32_t* rawBuf = NULL;
	int rawY = 0;
	if (rawBase[0]) dst = raw_row_at(dst, &rawY, &rawBuf); /* (raw_picture's rows may be further apart than armGBA counts) */
#endif
	for (int i = 0; i < GBA_W / 2; i += 2) {
		uint32_t const a = words[i], b = words[i + 1];
		dst[2 * i] = widenLow[a & 0xFF] | widenHigh[(a >> 8) & 0xFF];
		dst[2 * i + 1] = widenLow[(a >> 16) & 0xFF] | widenHigh[a >> 24];
		dst[2 * i + 2] = widenLow[b & 0xFF] | widenHigh[(b >> 8) & 0xFF];
		dst[2 * i + 3] = widenLow[(b >> 16) & 0xFF] | widenHigh[b >> 24];
	}
#ifdef __3DS__
	if (rawBuf) raw_row_done(rawBuf, rawY);
#endif
}

/* A whole picture; two colors a word where they are word aligned (the ARM11 can't load a half-word pair otherwise). */
static void widen_picture(uint32_t* dst, const uint16_t* src) {
	widen_tables();
	if (!((uintptr_t) src & 3)) {
		widen_words(dst, __builtin_assume_aligned(src, 4), widenLow, widenHigh);
		return;
	}
	for (int i = 0; i < GBA_W * GBA_H; ++i) dst[i] = widen(src[i]);
}

/* armGBA's picture is the GBA's own 15-bit colours; GBA DS wants mGBA's
 * 32-bit ones (bytes R, G, B, X), widened the way mGBA widens them. */
#define MARK_555 0x7C1F
static void take_picture(Emu* e) {
	ArmCore* c = AC(e);
	const uint32_t* wide = c->wideBuf[0] ? renderThreadWide(c->gba) : NULL;
	e->fbGpu = NULL;
	e->fbGpuDirty = 0;
	e->fbPitch = GBA_W;
	e->fbCompactOk = false;
	if (wide && c->wideLinear && c->widePitch != GBA_W && c->markFrame) {
		/* (the marks are put on rows of GBA_W: on a copy) */
		for (int y = 0; y < GBA_H; ++y) memcpy(&c->fbOwn[y * GBA_W], &wide[y * c->widePitch], GBA_W * sizeof(uint32_t));
		e->fb = c->fbOwn;
	} else if (wide) {
		e->fb = (uint32_t*) wide;
		if (c->twin[0] && !c->markFrame) { /* (a marked picture's fourth bytes are not the twin's) */
			e->fbGpu = c->twin[wide == c->wideBuf[1]];
			e->fbGpuStride = EMU_TWIN_STRIDE;
		} else if (c->wideLinear && !c->markFrame) {
			e->fbGpu = wide;
			e->fbGpuStride = c->widePitch;
			e->fbPitch = c->widePitch;
		}
	} else {
		e->fb = c->fbOwn;
		widen_picture(e->fb, (const uint16_t*) c->gba->renderer.frame);
	}
	/* (emu_mark_backdrop: the backdrop's pixels told apart by their fourth byte, as mGBA's flags there tell them apart) */
	const uint16_t* mark = AC(e)->markFrame;
	for (int i = 0; mark && i < GBA_W * GBA_H; ++i) {
		if (mark[i] == MARK_555) e->fb[i] = (e->fb[i] & 0x00FFFFFFu) | 0xFC000000u;
	}
	/* (the marks are in the fourth bytes, which emu_bright doesn't read) */
	c->bright = wide ? bright_sum(wide == c->wideBuf[1]) : -1;
	e->fbPainted = false;
}

static void run_frame(Emu* e, uint32_t keys, bool draw) {
	if (emu_cancelled(e)) return;
	ArmCore* c = AC(e);
	struct GBA* gba = c->gba;
#ifdef __3DS__
	/* The copy's thread runs the routines the game's thread wrote at open; its core's instruction cache may still hold the
	 * last game's code at those addresses. */
	int const core = host_core_now();
	if (core != c->codeCore) {
		bool const emptied = host_icache_invalidate();
		logLine("instruction cache of core %d %s (the code was written on core %d)", core, emptied ? "emptied" : "left alone", c->codeCore);
		c->codeCore = core;
	}
#endif
	if (!draw && e->fb != c->fbOwn) {
		/* (the picture kept is a worker's buffer, which it writes again when this frame ends: copied before) */
		const int pitch = emu_pitch(e);
		for (int y = 0; y < GBA_H; ++y) memcpy(&c->fbOwn[y * GBA_W], &e->fb[y * pitch], GBA_W * sizeof(uint32_t));
		e->fb = c->fbOwn;
		e->fbPitch = GBA_W;
		e->fbCompactOk = false;
		e->fbGpu = NULL;
	}
	if (!draw) c->bright = -1;
	++e->frames;
	gbaSetKeys(gba, (uint16_t) keys);
	gba->gpio.luminance = solar_reading();
	videoSetDrawSkipped(!draw);
	uint32_t const frame = gba->video.frameCounter;
#ifdef __3DS__
	/* (each machine, for its own frames: past 1.5 ms of compiling in a frame, code met for the
	 * first times is stepped through a few times more before it's
	 * compiled, and compiled in a later frame if it doesn't come round
	 * again; a Mario Kart race frame has about that much to spare: 4 ms
	 * made 18-22 ms frames) */
	jitSetCompileBudget(gba, c->compileBudget);
#endif
	do {
		gbaRunPart(gba);
	} while (gba->video.frameCounter == frame);
	videoSetDrawSkipped(false);
	audioThreadKick(gba);
	if (c->cheatsOn) cheatsApply(&c->cheats, gba);
#ifdef __3DS__
	bool const timed = jitStatsOn && c->game && draw; /* (the sums are the game thread's: a copy runs on another one) */
	uint64_t const t0 = timed ? renderThreadHostTicks() : 0;
#endif
	if (draw) take_picture(e);
#ifdef __3DS__
	uint64_t const t1 = timed ? renderThreadHostTicks() : 0;
#endif
	if (draw) split_frame(c);
#ifdef __3DS__
	if (timed) {
		pictureTicks += t1 - t0;
		splitTicks += renderThreadHostTicks() - t1;
	}
#endif
	audioThreadTake(gba);
#ifdef __3DS__
	++c->ran;
	jit_stats(c, c->game ? gba->video.frameCounter : c->ran, c->game, false);
#endif
	if (c->ring) {
		const int16_t* in = (const int16_t*) gba->audio.out;
		for (uint32_t i = 0; i < gba->audio.outCount; ++i) {
			if (c->ringCount == c->ringCap) { /* nobody's reading: the oldest goes */
				c->ringHead = (c->ringHead + 1) & (c->ringCap - 1);
				--c->ringCount;
			}
			size_t at = (c->ringHead + c->ringCount) & (c->ringCap - 1);
			c->ring[at * 2] = in[i * 2];
			c->ring[at * 2 + 1] = in[i * 2 + 1];
			++c->ringCount;
		}
	}
	gba->audio.outCount = 0;
	save_tick(c);
}

void emu_tidy(Emu* e) {
	ArmCore* c = AC(e);
	size_t size = 0;
	size_t used = c && c->gba ? jitCodeUsed(c->gba, &size) : 0;
	/* (three quarters: since armGBA reuses a full stretch part's room and caps careful
	 * copies, Mario Kart's code levels off at about 60% of its 13 MiB, and emptying it at
	 * 40% only cost a 120 ms frame between races, 2026-11-16) */
	if (size && (used > size / 4 * 3 || getenv("GBADS_TIDY_ALWAYS"))) { /* (GBADS_TIDY_ALWAYS: tests) */
		/* (what it had compiled goes back on the warm list, for the frames'
		 * spare time to compile again before the race) */
		enum { MOST = 1 << 16 };
		uint32_t* list = malloc((size_t) MOST * 8);
		size_t n = list ? jitWarmTake(c->gba, list, MOST) : 0;
		jitFlush(c->gba);
		if (n) jitWarmGive(c->gba, list, n);
		free(list);
		fprintf(stderr, "code memory emptied off the race: %lu of %lu KiB used, %lu blocks to compile again\n", (unsigned long) (used >> 10),
		        (unsigned long) (size >> 10), (unsigned long) n);
	}
}

bool emu_idle_step(Emu* e) {
#ifdef __3DS__
	ArmCore* c = AC(e);
	size_t size = 0;
	/* (the list into at most 20% of the code memory: the rest for what the
	 * game compiles as it goes, 7-9 MiB in a race of Mario Kart's; armGBA's
	 * own limit is half) */
	if (c && c->gba && c->ring && jitCodeUsed(c->gba, &size) > size / 10 * 2) return false;
	return c && c->gba && c->ring && jitWarmStep(c->gba);
#else
	(void) e;
	return false;
#endif
}

void emu_frame(Emu* e, uint32_t keys) {
	run_frame(e, keys, true);
}

/* (armGBA draws with its own threads, set up by its frontend options, not mGBA's proxy) */
void emu_threaded_video(bool on) {
	(void) on;
}

/* (the bench's "video wait": the game thread waiting for the renderer's worker, in microseconds) */
uint32_t emu_video_wait_us(void) {
#ifdef __3DS__
	extern uint64_t renderWaitTicks;
	return (uint32_t) (renderWaitTicks * 1000 / 268112);
#else
	return 0;
#endif
}

void emu_idle_loops(const char* mode) {
	(void) mode;
}

bool emu_threaded(const Emu* e) {
	(void) e;
	return false;
}

void emu_frame_blind(Emu* e, uint32_t keys) {
	run_frame(e, keys, false);
}

void emu_reset(Emu* e) {
	gbaReset(AC(e)->gba, true);
}

size_t emu_audio_read(Emu* e, int16_t* out, size_t maxFrames) {
	ArmCore* c = AC(e);
	if (!c->ring) return 0;
	size_t n = c->ringCount < maxFrames ? c->ringCount : maxFrames;
	for (size_t i = 0; i < n; ++i) {
		size_t at = (c->ringHead + i) & (c->ringCap - 1);
		out[i * 2] = c->ring[at * 2];
		out[i * 2 + 1] = c->ring[at * 2 + 1];
	}
	c->ringHead = (c->ringHead + n) & (c->ringCap - 1);
	c->ringCount -= n;
	return n;
}

unsigned emu_audio_rate(Emu* e) {
	(void) e;
	return 32768;
}

/* ---------------------------------------------- states */

size_t emu_state_size(Emu* e) {
	(void) e;
	return gbaStateSize();
}

bool emu_state_save(Emu* e, void* buf) {
	gbaSaveState(AC(e)->gba, buf);
	return true;
}

/* The core's layer switches aren't the machine's: a state leaves them. */
static void layers_apply(ArmCore* c);

bool emu_state_load(Emu* e, const void* buf) {
	ArmCore* c = AC(e);
	/* (the borrowed cartridge is this core's own business, not the state's) */
	bool ok = gbaLoadState(c->gba, buf, gbaStateSize());
#ifdef __3DS__
	++c->loads;
#endif
	layers_apply(c);
	return ok;
}

/* A state file: a header, armGBA's state (which holds the battery save as it
 * was), then GBA DS's own data (emu_state_save_file_as's extra). Not mGBA's
 * format: the other builds' states don't load here, nor these there. */
#define STATE_FILE_MAGIC 0x33534447u /* "GDS3" */
typedef struct {
	uint32_t magic;
	uint32_t romCrc;
	uint32_t stateSize;
	uint32_t extraSize;
} StateFileHeader;

/* (fmt: only armGBA's own here; a RetroArch state would hold an mGBA core's
 * state, which this isn't) */
bool emu_state_save_file_as(Emu* e, const char* path, const void* extra, size_t extraN, StateFormat fmt) {
	if (fmt != STATE_FORMAT_MGBA) {
		fprintf(stderr, "state %s: RetroArch's format isn't made on the 3DS (armGBA's states aren't mGBA's)\n", path);
		return false;
	}
	size_t n = gbaStateSize();
	void* buf = malloc(n);
	if (!buf) return false;
	gbaSaveState(AC(e)->gba, buf);
	StateFileHeader h = { STATE_FILE_MAGIC, e->romCrc, (uint32_t) n, extra ? (uint32_t) extraN : 0 };
	FILE* f = fopen(path, "wb");
	bool ok = f && fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(buf, 1, n, f) == n && (!extra || !extraN || fwrite(extra, 1, extraN, f) == extraN);
	if (f) ok = fclose(f) == 0 && ok;
	free(buf);
	return ok;
}

bool emu_state_save_file(Emu* e, const char* path) {
	return emu_state_save_file_as(e, path, NULL, 0, STATE_FORMAT_MGBA);
}

/* armGBA's own states (GBA DS 3DS's) load; any other file is another
 * emulator's to this build */
StateFileKind emu_state_file_kind(Emu* e, const char* path, bool* fromGbads) {
	(void) e;
	if (fromGbads) *fromGbads = false;
	FILE* f = fopen(path, "rb");
	if (!f) return STATE_FILE_NONE;
	StateFileHeader h;
	bool ours = fread(&h, sizeof(h), 1, f) == 1 && h.magic == STATE_FILE_MAGIC;
	fclose(f);
	if (!ours) return STATE_FILE_OTHER_CORE;
	if (fromGbads) *fromGbads = h.extraSize > 0;
	return h.stateSize == gbaStateSize() ? STATE_FILE_MGBA : STATE_FILE_UNREADABLE;
}

bool emu_state_load_file(Emu* e, const char* path) {
	ArmCore* c = AC(e);
	StateFileHeader h;
	FILE* f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "state %s: can't be opened\n", path);
		return false;
	}
	void* buf = NULL;
	bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == STATE_FILE_MAGIC && h.stateSize == gbaStateSize() &&
	          (buf = malloc(h.stateSize)) && fread(buf, 1, h.stateSize, f) == h.stateSize;
	fclose(f);
	if (!ok)
		fprintf(stderr, "state %s: not loaded (%s)\n", path,
		        h.magic != STATE_FILE_MAGIC ? "not a GBA DS 3DS state" : h.stateSize != gbaStateSize() ? "made by another version" : !buf ? "no memory" : "cut short");
	if (ok) {
		/* The battery save stays as it is now: a state never puts an old
		 * save back (mGBA's load left it alone too). */
		size_t keep = sizeof(c->gba->savedata.storage);
		uint8_t* save = c->savePath ? malloc(keep) : NULL;
		if (save) memcpy(save, c->gba->savedata.storage, keep);
		ok = gbaLoadState(c->gba, buf, h.stateSize);
		if (save) {
			memcpy(c->gba->savedata.storage, save, keep);
			free(save);
		}
		layers_apply(c);
		if (!ok) fprintf(stderr, "state %s: the core refused it\n", path);
	}
	free(buf);
	return ok;
}

bool emu_state_file_rom_crc(const char* path, uint32_t* crc) {
	StateFileHeader h;
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == STATE_FILE_MAGIC;
	fclose(f);
	if (ok) *crc = h.romCrc;
	return ok;
}

void* emu_state_file_get_extra(Emu* e, const char* path, size_t* n) {
	(void) e;
	*n = 0;
	StateFileHeader h;
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	void* out = NULL;
	if (fread(&h, sizeof(h), 1, f) == 1 && h.magic == STATE_FILE_MAGIC && h.extraSize && h.extraSize < 16 * 1024 * 1024 &&
	    fseek(f, (long) (sizeof(h) + h.stateSize), SEEK_SET) == 0 && (out = malloc(h.extraSize))) {
		if (fread(out, 1, h.extraSize, f) == h.extraSize) *n = h.extraSize;
		else {
			free(out);
			out = NULL;
		}
	}
	fclose(f);
	return out;
}

/* ---------------------------------------------- rewind
 * Whole states in a ring (armGBA has no rewind of its own). A state is
 * 0.8 MB, so the ring is kept short; on the console there's none
 * (EMU_REWIND_MAX 0: the memory is the game's and the recompiler's). */
#ifndef EMU_REWIND_MAX
#ifdef __3DS__
#define EMU_REWIND_MAX 0
#else
#define EMU_REWIND_MAX 64
#endif
#endif
static struct {
	uint8_t** states;
	size_t cap, head, count;
} rw;

void emu_rewind_deinit(void) {
	if (!rewindOwner) return;
	for (size_t i = 0; i < rw.cap; ++i) free(rw.states[i]);
	free(rw.states);
	memset(&rw, 0, sizeof(rw));
	rewindOwner = NULL;
}

void emu_rewind_init(Emu* e, size_t entries) {
	emu_rewind_deinit();
	if (entries > EMU_REWIND_MAX) entries = EMU_REWIND_MAX;
	if (!entries) return;
	rw.states = calloc(entries, sizeof(*rw.states));
	if (!rw.states) return;
	rw.cap = entries;
	rewindOwner = e;
}

void emu_rewind_append(Emu* e) {
	if (rewindOwner != e) return;
	size_t at = (rw.head + rw.count) % rw.cap;
	if (rw.count == rw.cap) {
		at = rw.head;
		rw.head = (rw.head + 1) % rw.cap;
		--rw.count;
	}
	if (!rw.states[at] && !(rw.states[at] = malloc(gbaStateSize()))) return;
	gbaSaveState(AC(e)->gba, rw.states[at]);
	++rw.count;
}

bool emu_rewind_step(Emu* e) {
	if (rewindOwner != e || !rw.count) return false;
	size_t at = (rw.head + rw.count - 1) % rw.cap;
	--rw.count;
	return emu_state_load(e, rw.states[at]);
}

size_t emu_rewind_count(void) {
	return rewindOwner ? rw.count : 0;
}

/* ---------------------------------------------- the game's memory */

uint8_t* emu_memory(Emu* e, int region, size_t* size) {
	struct GBA* gba = e->core ? AC(e)->gba : NULL;
	*size = 0;
	if (!gba) return NULL;
	switch (region) {
	case 2:
		*size = SIZE_EWRAM;
		return gba->ewram;
	case 3:
		*size = SIZE_IWRAM;
		return gba->iwram;
	case 0xE:
		*size = save_size(gba);
		return *size ? gba->savedata.storage : NULL;
	}
	return NULL;
}

/* Reads see memory as it is, with no side effects (the save memory's chips
 * aren't driven: its bytes are read where they lie). */
/* The write-only I/O registers (the backgrounds' offsets and affine
 * parameters, the windows, mosaic, the blend's fade, the DMA addresses),
 * which a read on the bus gives the open bus for. */
static bool io_write_only(uint32_t off) {
	return (off >= 0x010 && off <= 0x046) || off == 0x04C || off == 0x054 || (off >= 0x0A0 && off <= 0x0A7) ||
	       (off >= 0x0B0 && off <= 0x0B7) || (off >= 0x0BC && off <= 0x0C3) || (off >= 0x0C8 && off <= 0x0CF) ||
	       (off >= 0x0D4 && off <= 0x0DB);
}

uint8_t emu_read8(Emu* e, uint32_t a) {
	struct GBA* gba = AC(e)->gba;
	if (a >> 24 == 0xE) return gba->savedata.storage[a & (SIZE_FLASH1M - 1)];
	/* Profiles read registers as mGBA's debug view gives them (src/emu.c's
	 * rawRead): a write-only one as last written, not the open bus. */
	if (a >> 24 == 0x4 && io_write_only(a & 0xFFFFFF)) return (uint8_t) (gba->io[(a & 0x3FE) >> 1] >> ((a & 1) * 8));
	return (uint8_t) (memoryView32(gba, a & ~3u) >> ((a & 3) * 8));
}
uint16_t emu_read16(Emu* e, uint32_t a) {
	return (uint16_t) (emu_read8(e, a) | emu_read8(e, a + 1) << 8);
}
uint32_t emu_read32(Emu* e, uint32_t a) {
	return (uint32_t) emu_read16(e, a) | (uint32_t) emu_read16(e, a + 2) << 16;
}

/* A core that writes to the cartridge (a profile's ROM patch, for that copy
 * only) stops reading the buffer it shares and gets one of its own. */
static bool rom_own(ArmCore* c) {
	if (!c->shared) return true;
	struct GBA* gba = c->gba;
	pthread_mutex_lock(&sharedLock);
	bool ok = true;
	if (c->shared->users > 1) {
#ifdef __3DS__
		size_t n = gbaRomBufferSize(gba->romSize);
#else
		size_t n = SIZE_ROM;
#endif
		uint8_t* own = calloc(1, n);
		ok = own != NULL;
		if (ok) {
			memcpy(own, gba->rom, gba->romSize);
			gba->rom = own;
			--c->shared->users;
			c->shared = NULL;
		}
	} else {
#ifndef __3DS__
		memset(c->shared, 0, sizeof(*c->shared)); /* its only user: the buffer is this core's again */
		c->shared = NULL;
#endif
		/* (the console: still lent, patch and all - a profile's patches are
		 * made by every core on the game, and a second cartridge isn't to
		 * be had) */
	}
	pthread_mutex_unlock(&sharedLock);
	return ok;
}

/* The cartridge is written where it lies; the rest through armGBA's stores,
 * so the renderer and the recompiler hear of it. */
static bool rom_write(ArmCore* c, uint32_t a, uint32_t v, int n) {
	struct GBA* gba = c->gba;
	if (a >> 24 < 8 || a >> 24 > 0xD) return false;
	uint32_t off = a & (SIZE_ROM - 1);
	if (gba->rom && off + (uint32_t) n <= gba->romSize) {
		/* (a patch another core on this cartridge already made: nothing) */
		bool same = true;
		for (int i = 0; i < n; ++i) same = same && gba->rom[off + (uint32_t) i] == (uint8_t) (v >> (8 * i));
		if (same) return true;
		if (!rom_own(c)) return true;
		for (int i = 0; i < n; ++i) gba->rom[off + (uint32_t) i] = (uint8_t) (v >> (8 * i));
		jitInvalidate(gba, a, (uint32_t) n);
	}
	return true;
}
void emu_write8(Emu* e, uint32_t a, uint8_t v) {
	int cycles = 0;
	if (!rom_write(AC(e), a, v, 1)) memoryStore8(AC(e)->gba, a, v, &cycles);
}
void emu_write16(Emu* e, uint32_t a, uint16_t v) {
	int cycles = 0;
	if (!rom_write(AC(e), a, v, 2)) memoryStore16(AC(e)->gba, a, v, &cycles);
}
void emu_write32(Emu* e, uint32_t a, uint32_t v) {
	int cycles = 0;
	if (!rom_write(AC(e), a, v, 4)) memoryStore32(AC(e)->gba, a, v, &cycles);
}

uint16_t emu_io16(Emu* e, uint32_t offset) {
	return AC(e)->gba->io[(offset & 0x3FE) >> 1];
}

const uint16_t* emu_vram(Emu* e) {
	return (const uint16_t*) AC(e)->gba->vram;
}

/* As mGBA's (src/emu.c): the bytes as they are. Through the bus a halfword at
 * a time: a byte stored to background VRAM is written to both halves of its
 * halfword, as on the GBA, which garbled the tiles Sonic Pinball Party's
 * copies carry over (tall.live.carry). An odd byte at either end keeps its
 * neighbour. */
void emu_vram_write(Emu* e, uint32_t off, const void* src, size_t n) {
	struct GBA* gba = AC(e)->gba;
	const uint8_t* p = src;
	int cycles = 0;
	if (off >= SIZE_VRAM) return;
	if (n > SIZE_VRAM - off) n = SIZE_VRAM - off;
	for (size_t i = 0; i < n;) {
		uint32_t const a = off + (uint32_t) i;
		uint16_t half = (uint16_t) (gba->vram[a & ~1u] | gba->vram[(a & ~1u) + 1] << 8);
		if (a & 1) {
			half = (uint16_t) ((half & 0x00FF) | p[i] << 8);
			i += 1;
		} else if (i + 1 < n) {
			half = (uint16_t) (p[i] | p[i + 1] << 8);
			i += 2;
		} else {
			half = (uint16_t) ((half & 0xFF00) | p[i]);
			i += 1;
		}
		memoryStore16(gba, 0x06000000 + (a & ~1u), half, &cycles);
	}
}

void emu_oam_read(Emu* e, uint16_t* out) {
	memcpy(out, AC(e)->gba->oam, SIZE_OAM);
}

void emu_oam_write(Emu* e, const uint16_t* src) {
	int cycles = 0;
	for (uint32_t i = 0; i < SIZE_OAM / 2; ++i) memoryStore16(AC(e)->gba, 0x07000000 + i * 2, src[i], &cycles);
}

/* ---------------------------------------------- what's drawn */

/* armGBA's renderer asks this of each sprite as it lists them (its
 * RenderHooks, third_party/armgba src/video/render.h). */
static bool rules_match(const ArmCore* c, uint16_t a, uint16_t b, uint16_t attrC) {
	int x = (int) ((uint32_t) (b & 0x1FF) << 23) >> 23;
	int y = a & 0xFF;
	if (y >= 160) y -= 256;
	int tile = attrC & 0x3FF;
	int prio = (attrC >> 10) & 3;
	int pal = attrC >> 12;
	bool transformed = (a >> 8) & 1;
	int shape = (a >> 14) & 3, size = (b >> 14) & 3;
	for (int i = 0; i < c->nRules; ++i) {
		const SpriteRule* r = &c->rules[i];
		if (x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1 && tile >= r->tileMin && tile <= r->tileMax &&
		    (r->prio < 0 || prio == r->prio) && (!r->palMask || (r->palMask >> pal & 1)) &&
		    (!r->tagged || (!transformed && (b & r->tagged))) && (!r->shapes || (r->shapes >> (shape * 4 + size) & 1))) {
			return true;
		}
	}
	return false;
}

/* emu_hide_grouped (src/emu.c's hide_grouped; Lady Sia): of the sprites the
 * rules match, only those in the run of matching sprites at the start of the
 * table, or in a run of sprites in one palette that all match, are hidden.
 * armGBA tells the frontend the table as the game picture's sprites are listed
 * (spriteTable, on the thread that draws), then asks about each listed sprite
 * in order (sprite_hidden): the list and its choices are made here as armGBA
 * will list them, kept per thread. */
static const uint8_t objSizes[16][2] = { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 }, { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 },
	                                     { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
static _Thread_local struct {
	const ArmCore* core;
	int n, next;
	uint16_t attr[128][3];
	bool hide[128];
} groups;

static void sprite_table(void* context, const uint8_t* oam) {
	const ArmCore* c = context;
	groups.core = c;
	groups.n = groups.next = 0;
	if (!c->grouped || c->keepOnly || !c->nRules) return;
	bool match[128];
	int pal[128];
	/* the sprites armGBA lists (render.c cleanOam): switched on, and on the
	 * screen's lines and columns */
	for (int i = 0; i < 128; ++i) {
		uint16_t a = (uint16_t) (oam[i * 8] | oam[i * 8 + 1] << 8);
		uint16_t b = (uint16_t) (oam[i * 8 + 2] | oam[i * 8 + 3] << 8);
		uint16_t cc = (uint16_t) (oam[i * 8 + 4] | oam[i * 8 + 5] << 8);
		bool transformed = (a >> 8) & 1;
		if (!transformed && ((a >> 9) & 1)) continue;
		int w = objSizes[((a >> 14) & 3) * 4 + ((b >> 14) & 3)][0];
		int h = objSizes[((a >> 14) & 3) * 4 + ((b >> 14) & 3)][1];
		int x = (int) ((uint32_t) (b & 0x1FF) << 23) >> 23;
		if (transformed) {
			int dbl = (a >> 9) & 1;
			w <<= dbl;
			h <<= dbl;
		} else if (x < 0 && x + w < 0) {
			continue;
		}
		if ((a & 0xFF) >= 160 && (a & 0xFF) + h < 228) continue;
		if ((b & 0x1FF) >= 240 && (b & 0x1FF) + w < 512) continue;
		int k = groups.n++;
		groups.attr[k][0] = a, groups.attr[k][1] = b, groups.attr[k][2] = cc;
		match[k] = rules_match(c, a, b, cc);
		pal[k] = cc >> 12;
		groups.hide[k] = false;
	}
	/* the run at the start */
	for (int k = 0; k < groups.n && match[k]; ++k) groups.hide[k] = true;
	/* runs in one palette */
	for (int k = 0; k < groups.n;) {
		int j = k;
		bool all = true;
		for (; j < groups.n && pal[j] == pal[k]; ++j) all = all && match[j];
		for (int m = k; m < j; ++m) groups.hide[m] = groups.hide[m] || all;
		k = j;
	}
}

/* armGBA's renderer asks this of each sprite as it lists them (its
 * RenderHooks, third_party/armgba src/video/render.h). */
static bool sprite_hidden(void* context, uint16_t a, uint16_t b, uint16_t attrC) {
	const ArmCore* c = context;
	for (int i = 0; i < c->nExact; ++i) {
		if (c->exact[i][0] == a && c->exact[i][1] == b && c->exact[i][2] == attrC) return !c->exactKeep;
	}
	if (!c->nRules && !c->keepOnly) return false;
	if (c->grouped && !c->keepOnly && groups.core == c) {
		/* the next listed sprite: found from where the last one was */
		for (int t = 0; t < groups.n; ++t) {
			int k = (groups.next + t) % groups.n;
			if (groups.attr[k][0] == a && groups.attr[k][1] == b && groups.attr[k][2] == attrC) {
				groups.next = k + 1;
				return groups.hide[k];
			}
		}
	}
	return rules_match(c, a, b, attrC) != c->keepOnly;
}

static void hooks_changed(ArmCore* c) {
	c->hooks.hideSprite = c->nRules || c->keepOnly || c->nExact ? sprite_hidden : NULL;
	if (c->markFrame) {
		c->hooks.secondHideLayers = c->hooks.hideLayers;
		c->hooks.secondHideSprite = c->hooks.hideSprite;
	}
	c->hooks.spriteTable = c->grouped ? sprite_table : NULL;
	c->hooks.context = c;
	++c->hooks.generation;
}

static void layers_apply(ArmCore* c) {
	uint8_t hide = ~c->layers & 0x1F;
	if (hide != c->hooks.hideLayers) {
		c->hooks.hideLayers = hide;
		hooks_changed(c);
	}
}

void emu_layers(Emu* e, unsigned mask) {
	AC(e)->layers = mask;
	layers_apply(AC(e));
}

void emu_hide_sprites(Emu* e, const SpriteRule* rules, int n) {
	ArmCore* c = AC(e);
	c->nRules = n > 12 ? 12 : n;
	if (c->nRules) memcpy(c->rules, rules, sizeof(*rules) * c->nRules);
	hooks_changed(c);
}

/* The opposite: only the sprites that match one of the rules are drawn (Lady
 * Sia's pause screen, src/emu.c). */
void emu_keep_sprites(Emu* e, const SpriteRule* rules, int n) {
	ArmCore* c = AC(e);
	c->nRules = n > 12 ? 12 : n;
	if (c->nRules) memcpy(c->rules, rules, sizeof(*rules) * c->nRules);
	c->keepOnly = n > 0;
	hooks_changed(c);
}

/* A matching sprite hidden only as part of a group (sprite_table above). */
void emu_hide_grouped(Emu* e, bool on) {
	ArmCore* c = AC(e);
	c->grouped = on;
	hooks_changed(c);
}

void emu_hide_exact(Emu* e, const uint16_t (*attrs)[3], int n) {
	ArmCore* c = AC(e);
	c->nExact = n > 128 ? 128 : n;
	c->exactKeep = false;
	if (c->nExact) memcpy(c->exact, attrs, sizeof(*attrs) * c->nExact);
	hooks_changed(c);
}

/* ---------------------------------------------- the HUD split
 *
 * As src/emu.c's, on armGBA's second picture (RenderHooks.second: a renderer
 * of its own beside the game's, fed the same registers and video memory):
 * it draws only the HUD's layers and sprites, on a backdrop of the 'nothing
 * here' colour, while the game's picture is drawn without them. The layers
 * are decided line by line (split_line, called by whichever thread draws)
 * from the video registers as that line has them. */
#define HUD_CLEAR_555 0x0401
/* (the 3DS draws on threads a frame late: renderThreadStart's late) */
#ifdef __3DS__
#define SPLIT_LATE 1
#else
/* (GBADS_RENDER_LATE=1, tests: drawn a frame late as the console's game is) */
#define SPLIT_LATE (getenv("GBADS_RENDER_LATE") != NULL) /* (the HUD split is the game's own core's) */
#endif

static struct {
	ArmCore* core; /* the core it's on (NULL: not installed) */
	bool active;
	unsigned bgMask;
	unsigned gameHide; /* BGs the game's picture is drawn without now */
	unsigned hudMask;  /* the second picture's BGs, and whether it has sprites */
	bool hudObj;
	SpriteRule objs[12];
	int nObjs;
	uint16_t keep[128][3]; /* sprites the HUD also draws by their exact attributes */
	int nKeep;
	uint32_t* hudBuf;
	uint32_t* grabBuf;
	unsigned grabMask;
	uint16_t grabCnt;
	bool grabbing, grabbed;
	uint32_t grabNeed; /* (hooks.secondCount the grab's picture has) */
	uint32_t hudNeed; /* (and the first HUD picture after the split changed) */
	int prioBelow;
	unsigned keepBgs;
	unsigned baseMask, memLayers;
	uint32_t memAddr[2], memMask[2], memVal[2];
	int memSize[2], memN;
	bool memAny;
	int memGamePal;
	unsigned memLineOff;
	unsigned lineHud, lineGame;
	int bandY0, bandY1;
	uint16_t frame[GBA_W * GBA_H];
} split;

/* Logo extraction (src/logo.c) tells the logo's layer from the backdrop
 * behind it by comparing two pictures' pixels as a whole: on mGBA their
 * fourth byte holds the software renderer's flags (FLAG_UNWRITTEN, 0xFC, on
 * the backdrop; the layer, priority and blend targets where a layer drew),
 * so a layer's pixel the colour of the backdrop (Sonic Advance 3's white
 * glow on a white backdrop) still counts as the logo's. armGBA's picture
 * has no such byte: with this on, each frame is drawn a second time (the
 * renderer's second picture, the same layers and sprites) on a backdrop of
 * MARK_555, never brightened or darkened, and the pixels left that colour
 * there get 0xFC in their fourth byte, the others 0xFF. Not for a core with
 * the HUD split (it has the second picture). On for the few frames a
 * capture compares (src/logo.c, profiles.c's aw_clean...), as it doubles
 * the drawing. */
void emu_mark_backdrop(Emu* e, bool on) {
	ArmCore* c = AC(e);
	if (!c || split.core == c || on == (c->markFrame != NULL)) return;
	if (on) {
		c->markFrame = malloc(GBA_W * GBA_H * sizeof(uint16_t));
		if (!c->markFrame) return;
		for (int i = 0; i < GBA_W * GBA_H; ++i) c->markFrame[i] = MARK_555;
		c->hooks.secondFrame = c->markFrame;
		c->hooks.secondBackdropSet = true;
		c->hooks.secondBackdrop = MARK_555;
		c->hooks.secondEvery = 1;
		c->hooks.second = true;
	} else {
		c->hooks.second = false;
		c->hooks.secondBackdropSet = false;
		c->hooks.secondFrame = NULL;
		c->hooks.secondHideSprite = NULL;
		c->hooks.secondHideLayers = 0;
		free(c->markFrame);
		c->markFrame = NULL;
		/* (the second picture's machine, about 770 KiB, given back: hooks
		 * set again on this core and its render thread's copies) */
		renderSetHooks(c->gba, NULL);
		renderSetHooks(c->gba, &c->hooks);
	}
	hooks_changed(c);
}
static uint32_t gameRegAddr;
static uint16_t gameRegBits;
static bool bandOn, bandCopy, bandWatch;
static int lastBandY0, lastBandY1 = -1;
static uint16_t bandNormal[4];
static int copyAbove;
static int splitRows;
static bool splitCopying;

static uint32_t colour_8888(uint16_t v) {
	uint32_t c = ((v << 3) & 0xF8) | ((v << 6) & 0xF800) | ((v << 9) & 0xF80000);
	return c | ((c >> 5) & 0x070707) | 0xFF000000u;
}

/* the second picture's sprites: the HUD's only */
static bool second_hidden(void* context, uint16_t a, uint16_t b, uint16_t attrC) {
	(void) context;
	for (int i = 0; i < split.nKeep; ++i) {
		if (split.keep[i][0] == a && split.keep[i][1] == b && split.keep[i][2] == attrC) return false;
	}
	int x = (int) ((uint32_t) (b & 0x1FF) << 23) >> 23;
	int y = a & 0xFF;
	if (y >= 160) y -= 256;
	int tile = attrC & 0x3FF;
	int prio = (attrC >> 10) & 3;
	int pal = attrC >> 12;
	bool transformed = (a >> 8) & 1;
	int shape = (a >> 14) & 3, size = (b >> 14) & 3;
	for (int i = 0; i < split.nObjs; ++i) {
		const SpriteRule* r = &split.objs[i];
		if (x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1 && tile >= r->tileMin && tile <= r->tileMax &&
		    (r->prio < 0 || prio == r->prio) && (!r->palMask || (r->palMask >> pal & 1)) &&
		    (!r->tagged || (!transformed && (b & r->tagged))) && (!r->shapes || (r->shapes >> (shape * 4 + size) & 1))) {
			return false;
		}
	}
	return true;
}

static void split_layers(unsigned bgMask, bool obj) {
	split.hudMask = bgMask & 0xF;
	split.hudObj = obj;
}
static void game_hide(unsigned bits, bool off) {
	split.gameHide = off ? split.gameHide | bits : split.gameHide & ~bits;
}

static unsigned bgs_with_cnt(const uint16_t* io, uint16_t cnt) {
	unsigned m = 0;
	for (int i = 0; i < 4; ++i) m |= (unsigned) (io[(0x08 >> 1) + i] == cnt) << i;
	return m;
}
static unsigned bgs_below(const uint16_t* io, int p) {
	unsigned m = 0;
	for (int i = 0; i < 4; ++i) m |= (unsigned) ((io[(0x08 >> 1) + i] & 3) < p) << i;
	return m;
}

/* a byte, halfword or word of EWRAM, IWRAM, the I/O registers or VRAM, as
 * the game has it now (0 elsewhere) */
static uint32_t split_read(uint32_t a, int size) {
	const struct GBA* g = split.core->gba;
	const uint8_t* p;
	switch (a >> 24) {
	case 2: p = g->ewram + (a & 0x3FFFF); break;
	case 3: p = g->iwram + (a & 0x7FFF); break;
	case 4: p = (const uint8_t*) g->io + (a & 0x3FF); break;
	case 6: p = g->vram + ((a & 0x1FFFF) < 0x18000 ? a & 0x1FFFF : (a & 0x1FFFF) - 0x8000); break;
	default: return 0;
	}
	uint32_t v = 0;
	memcpy(&v, p, size == 1 || size == 2 ? (size_t) size : 4);
	return v;
}

/* Whether text BG `bg`'s map has a tile in palette `pal` on screen line y */
static bool bg_line_has_pal(const uint16_t* io, const uint8_t* vramBytes, int bg, int y, int pal) {
	int mode = io[0] & 7;
	if (mode > 1 || (mode == 1 && bg > 1)) return false;
	uint16_t cnt = io[(0x08 >> 1) + bg];
	int hofs = io[(0x10 >> 1) + bg * 2] & 0x1FF, vofs = io[(0x12 >> 1) + bg * 2] & 0x1FF;
	int size = cnt >> 14, sb = (cnt >> 8) & 0x1F;
	int wide = size & 1, tall = size >> 1;
	int ty = ((y + vofs) >> 3) & (tall ? 63 : 31);
	for (int c = 0; c < GBA_W / 8 + 1; ++c) {
		int tx = ((hofs >> 3) + c) & (wide ? 63 : 31);
		int block = sb + (tx >> 5) + ((ty >> 5) << wide);
		uint32_t at = ((uint32_t) (block * 0x400 + (ty & 31) * 32 + (tx & 31)) * 2) & 0x1FFFE;
		if (at >= 0x18000) continue;
		uint16_t e2;
		memcpy(&e2, vramBytes + at, 2);
		if ((e2 & 0x3FF) && (e2 >> 12) == pal) return true;
	}
	return false;
}

/* Before each line is drawn: which layers the two pictures have on it. */
static void split_line(void* context, struct RenderLine* l) {
	ArmCore* c = context;
	if (c != split.core || !split.active) return;
	const uint16_t* io = l->io;
	int y = l->y;
	bool objs = split.nObjs > 0 || split.nKeep > 0;
	if (copyAbove) game_hide(split.bgMask, y >= copyAbove);
	if (splitRows && !splitCopying) {
		/* the HUD's layers leave the game only on its rows; below, the
		 * game draws them as it does, behind or in front of the rest */
		game_hide(split.bgMask, y < splitRows);
	}
	if (split.prioBelow) {
		/* the game moves its map and menus between layers (between frames
		 * and, as a panel slides, between lines): picked for each line */
		unsigned m = bgs_below(io, split.prioBelow) & ~split.keepBgs;
		if (m != split.bgMask) {
			split.bgMask = m;
			split.gameHide = m;
			if (!split.grabbing) split_layers(m, objs);
		}
	}
	if (y == 0 && split.memN) {
		/* emu_hud_mem_layers: decided as the frame starts drawing */
		bool match = !split.memAny;
		for (int i = 0; i < split.memN && match != split.memAny; ++i) match = (split_read(split.memAddr[i], split.memSize[i]) & split.memMask[i]) == split.memVal[i];
		unsigned m = split.baseMask | (match ? split.memLayers : 0);
		if (m != split.bgMask) {
			split.bgMask = split.lineHud = split.lineGame = m;
			split.gameHide = m;
			if (!split.grabbing) split_layers(m, objs);
		}
	}
	if (split.memGamePal) {
		/* on a line where one of those layers has a tile in that palette,
		 * the layer stays on the game there and is left out of the HUD */
		unsigned off = 0;
		for (int i = 0; i < 4; ++i) {
			if (((split.memLayers & split.bgMask) >> i) & 1 && bg_line_has_pal(io, l->vram, i, y, split.memGamePal - 1)) off |= 1u << i;
		}
		if (y == 0 || off != split.memLineOff) {
			split.gameHide = (split.gameHide & ~split.memLayers) | ((split.bgMask & ~off) & split.memLayers);
			if (!split.grabbing) split_layers(split.bgMask & ~off, objs);
			split.memLineOff = off;
		}
	}
	if (y == 0 && split.grabBuf) {
		if (split.grabCnt) split.grabMask = bgs_with_cnt(io, split.grabCnt);
		/* this frame: the grab layers into grabBuf, no sprites */
		/* (the first picture drawn wholly from here: drawn late, the one
		 * finished before this frame may not be counted yet; set once, as
		 * this line comes round each frame until the grab has its picture) */
		if (!split.grabbing) split.grabNeed = split.core->hooks.secondCount + (SPLIT_LATE ? 2 : 1);
		split.grabbing = true;
		split_layers(split.grabMask, false);
	}
	if (bandOn && !split.prioBelow) {
		/* the band: lines with sprites off (the rest of the screen has them
		 * on) where layers are set up differently from elsewhere: those
		 * layers go into the HUD, and out of the game unless they're
		 * copied. Not when last frame's band was most of the screen. */
		unsigned band = 0;
		static bool inBand;
		static int tail; /* lines of the box with the sprites back on */
		if (y == 0) split.bandY0 = GBA_H, split.bandY1 = -1, inBand = false;
		unsigned differs = 0;
		for (int i = 0; i < 4; ++i) {
			if (((io[0] >> (8 + i)) & 1) && bandNormal[i] && io[(0x08 >> 1) + i] != bandNormal[i]) differs |= 1u << i;
		}
		if (!(io[0] & 0x1000)) inBand = true, tail = 0;
		else inBand = inBand && differs && ++tail <= 8;
		if (!inBand) {
			for (int i = 0; i < 4; ++i) bandNormal[i] = io[(0x08 >> 1) + i];
		} else {
			if (lastBandY1 - lastBandY0 < 100 && !bandWatch) band = differs;
			split.bandY0 = y < split.bandY0 ? y : split.bandY0, split.bandY1 = y;
		}
		unsigned hud = split.bgMask | band, game = split.bgMask | (bandCopy ? 0 : band);
		if (game != split.lineGame) {
			split.lineGame = game;
			split.gameHide = game;
		}
		if (hud != split.lineHud) {
			split.lineHud = hud;
			if (!split.grabbing) split_layers(hud, objs);
		}
	}
	l->hideLayers |= split.gameHide & 0xF;
	l->secondHideLayers = (uint8_t) ((~split.hudMask & 0xF) | (split.hudObj ? 0 : 0x10));
}

/* After each frame of the core it's on: the second picture into the app's
 * buffer (or, once, the grab's). */
static void split_frame(ArmCore* c) {
	if (c != split.core || !split.active) return;
	/* (only a new picture: armGBA draws it one frame in hooks.secondEvery;
	 * a grab waits for the next one) */
	static uint32_t lastCount;
	if (c->hooks.secondEvery > 1 && c->hooks.secondCount == lastCount) return;
	lastCount = c->hooks.secondCount;
	if (split.grabbing && c->hooks.secondEvery > 1 && (int32_t) (c->hooks.secondCount - split.grabNeed) < 0) return;
	if (!split.grabbing && (int32_t) (c->hooks.secondCount - split.hudNeed) < 0) return;
	if (bandOn) lastBandY0 = split.bandY0, lastBandY1 = split.bandY1;
	uint32_t* dst = split.grabbing ? split.grabBuf : split.hudBuf;
	if (dst) widen_picture(dst, split.frame);
	if (!split.grabbing) hudFresh = true;
	if (split.grabbing) {
		split.grabbing = false;
		split.grabBuf = NULL;
		split.grabbed = true;
		split_layers(split.bgMask, split.nObjs > 0 || split.nKeep > 0);
	}
}

static void split_release(ArmCore* c) {
	if (split.core != c) return;
	c->hooks.second = false;
	c->hooks.line = NULL;
	memset(&split, 0, sizeof(split));
	/* (as src/emu.c's: Pokemon Pinball's tall view leaves these set, and the next game kept its HUD on both screens) */
	splitCopying = false;
	copyAbove = 0;
}

/* a video register written again as it stands, so both pictures take it
 * with or without the masked bits */
static void rewrite_register(struct GBA* gba, uint32_t addr) {
	uint16_t v = gba->io[addr >> 1];
	gba->io[addr >> 1] = renderWriteRegister(gba, addr, v);
	if (gba->renderThread) renderThreadRegister(gba, addr, v);
}

bool emu_hud_split(Emu* e, bool on, unsigned bgMask, const SpriteRule* objs, int nObjs, uint32_t* hud) {
	ArmCore* c = AC(e);
	if (!split.core) {
		if (!on) return true;
		memset(&split, 0, sizeof(split));
		splitCopying = false;
		split.core = c;
		split.hudBuf = hud;
		for (int i = 0; i < GBA_W * GBA_H; ++i) split.frame[i] = HUD_CLEAR_555;
		c->hooks.secondFrame = split.frame;
		c->hooks.secondHideSprite = second_hidden;
		c->hooks.secondBackdropSet = true;
		c->hooks.secondBackdrop = HUD_CLEAR_555;
		c->hooks.line = split_line;
#ifdef __3DS__
		/* the HUD drawn 12 times a second, not 60: the second screen
		 * doesn't need more, and the game does the time (the HUD's
		 * picture took the drawing thread about 3 ms a frame, and GBA DS
		 * 4.4 ms to make it and 3.2 to upload it each time). One frame in
		 * an odd number: F-Zero draws its map every other frame, and with
		 * 6 every picture fell on the same kind (after a state was loaded,
		 * the frames without it: no map) */
		c->hooks.secondEvery = 5;
#else
		/* (GBADS_SECOND_EVERY=6, tests: as the console draws it) */
		if (getenv("GBADS_SECOND_EVERY")) c->hooks.secondEvery = (uint8_t) atoi(getenv("GBADS_SECOND_EVERY"));
#endif
	}
	if (split.core != c) return false;
	if (on && (!split.active || bgMask != split.bgMask) && (SPLIT_LATE || c->hooks.secondEvery > 1)) {
		/* (switched on, or other layers: a picture drawn before this, or
		 * being drawn, isn't the HUD's; until the first drawn wholly after,
		 * the HUD is clear) */
		split.hudNeed = c->hooks.secondCount + (SPLIT_LATE ? 2 : 1);
		uint32_t clear = colour_8888(HUD_CLEAR_555);
		for (int i = 0; hud && i < GBA_W * GBA_H; ++i) hud[i] = clear;
	}
	split.hudBuf = hud;
	split.bgMask = split.baseMask = bgMask;
	split.lineHud = split.lineGame = bgMask;
	if (!on) lastBandY0 = 0, lastBandY1 = -1;
	split.nObjs = nObjs < 12 ? nObjs : 12;
	if (split.nObjs) memcpy(split.objs, objs, sizeof(*objs) * split.nObjs);
	/* the HUD: those layers and sprites only */
	if (!split.grabbing) split_layers(bgMask, split.nObjs > 0 || split.nKeep > 0);
	/* the game: without them (the rules can change while on: another scene) */
	split.gameHide = on ? bgMask & 0xF : 0;
	split.active = on;
	c->hooks.second = on;
	c->hooks.maskedRegister = (uint16_t) gameRegAddr;
	c->hooks.maskedBits = on ? gameRegBits : 0;
	emu_hide_sprites(e, on ? split.objs : NULL, on ? split.nObjs : 0); /* (and every line drawn again) */
	if (gameRegBits) rewrite_register(c->gba, gameRegAddr);
	return true;
}

void emu_hud_copy(Emu* e, bool copy) {
	if (!split.core || split.core != AC(e) || !split.active) return;
	splitCopying = copy;
	split.gameHide = copy ? 0 : split.bgMask & 0xF;
	emu_hide_sprites(e, copy ? NULL : split.objs, copy ? 0 : split.nObjs);
}

void emu_hud_keep_exact(const uint16_t (*attrs)[3], int n) {
	if (!split.core) return;
	n = n > 128 ? 128 : n;
	if (n) memcpy(split.keep, attrs, sizeof(*attrs) * (size_t) n);
	split.nKeep = n;
	if (!split.grabbing) split_layers(split.bgMask, split.nObjs > 0 || split.nKeep > 0);
	hooks_changed(split.core);
}

void emu_hud_copy_above(int row) {
	copyAbove = row;
}

void emu_hud_rows(int rows) {
	splitRows = rows;
}

void emu_hud_band(bool on, bool copy, bool watch) {
	if (on != bandOn) memset(bandNormal, 0, sizeof(bandNormal)), lastBandY0 = 0, lastBandY1 = -1;
	bandOn = on;
	bandCopy = copy;
	bandWatch = watch;
}

bool emu_hud_band_rows(int* y0, int* y1) {
	*y0 = lastBandY0, *y1 = lastBandY1;
	/* (a band that's most of the screen isn't one) */
	return lastBandY1 >= lastBandY0 && lastBandY1 - lastBandY0 < 100;
}

void emu_hud_game_reg(uint32_t addr, uint16_t bits) {
	gameRegAddr = addr;
	gameRegBits = bits;
}

uint32_t emu_hud_clear(void) {
	/* exactly what the second picture's backdrop comes out as */
	return split.core ? colour_8888(HUD_CLEAR_555) & 0xFFFFFF : 0xFF00FF;
}

void emu_hud_by_priority(int prioBelow) {
	split.prioBelow = prioBelow;
}

void emu_hud_keep_bgs(unsigned mask) {
	split.keepBgs = mask;
}

void emu_hud_mem_layers(const uint32_t addr[2], const uint32_t mask[2], const uint32_t value[2], const int size[2], int n, bool any,
                        unsigned layers, int gamePal) {
	split.memN = n < 0 ? 0 : n > 2 ? 2 : n;
	split.memGamePal = split.memN ? gamePal : 0;
	split.memLineOff = 0;
	split.memAny = any;
	for (int i = 0; i < split.memN; ++i) split.memAddr[i] = addr[i], split.memMask[i] = mask[i], split.memVal[i] = value[i], split.memSize[i] = size[i];
	split.memLayers = layers;
}

void emu_hud_grab(Emu* e, unsigned bgMask, uint32_t* buf) {
	(void) e;
	if (!split.core || !split.active) return;
	split.grabMask = bgMask;
	split.grabCnt = 0;
	split.grabBuf = buf;
	split.grabbed = false;
}

void emu_hud_grab_cnt(Emu* e, uint16_t cnt, uint32_t* buf) {
	emu_hud_grab(e, 0, buf);
	split.grabCnt = cnt;
}

int emu_frame_lag(Emu* e) {
	ArmCore* c = AC(e);
	return c && c->late ? 1 : 0;
}

int emu_hud_every(void) {
	return split.core && split.core->hooks.secondEvery > 1 ? split.core->hooks.secondEvery : 1;
}

bool emu_hud_fresh(void) {
	bool f = hudFresh || !split.core || split.core->hooks.secondEvery <= 1;
	hudFresh = false;
	return f;
}

bool emu_hud_grabbed(void) {
	bool g = split.grabbed;
	split.grabbed = false;
	return g;
}

/* ---------------------------------------------- cheats */

void emu_cheats_clear(Emu* e) {
	ArmCore* c = AC(e);
	cheatsClear(&c->cheats, c->gba);
	c->cheatsOn = false;
}

/* armGBA tells the formats apart by the lines' shape (eight and four hex
 * digits: Code Breaker; eight and eight: GameShark / Action Replay). */
int emu_cheats_add(Emu* e, const char* name, const char* const* codes, int n, int type) {
	(void) name;
	(void) type;
	ArmCore* c = AC(e);
	int taken = 0;
	for (int i = 0; i < n; ++i) {
		if (cheatsAdd(&c->cheats, codes[i])) ++taken;
	}
	if (taken) c->cheatsOn = true;
	return taken;
}

bool emu_has_solar(Emu* e) {
	ArmCore* c = e ? AC(e) : NULL;
	return c && c->gba && (c->gba->gpio.devices & HW_LIGHT_SENSOR) != 0;
}
