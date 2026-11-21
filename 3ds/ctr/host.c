/* host.h, the memory split, and the program's start: GBA DS's own main
 * (src/main.c, built as gbads_main) run with its folder on the SD card. */
#include "host.h"

#include <3ds.h>

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#define APP_DIR "/3ds/gba-ds"
#ifdef GBADS_BENCH_TAG
/* (each bench program its own folder: jobs, scripts, results, saves, states) */
#define BENCH_DIR "/3ds/gba-ds-bench-" GBADS_BENCH_TAG
#define BENCH_ROMS "/3ds/gba-ds-bench-roms"
#endif
#define ROM_DIR "/roms/gba"

/* The recompiler's helpers and the app's menus keep long paths on the stack. */
u32 __stacksize__ = 512 * 1024;

/* libctru splits the app's memory evenly between the heap and linear memory
 * (what the GPU and the DSP read), up to 32 MiB of linear. Here the heap
 * holds the game (up to 32 MiB), the recompiler's code for the game and its
 * hidden copy, and the copies themselves; the textures and the sound need
 * linear memory: it gets 12 MiB, and the heap the rest, as far as the
 * heap's address range goes. */
#define LINEAR_WANTED (12u << 20) /* (6.5 MiB of it in use in a game, 2026-11-12) */
extern char* fake_heap_start;
extern char* fake_heap_end;
u32 __ctru_heap;
u32 __ctru_heap_size;
u32 __ctru_linear_heap;
u32 __ctru_linear_heap_size;
void mappableInit(u32 addrMin, u32 addrMax);

void __system_allocateHeaps(void) {
	Handle limit = 0;
	s64 most = 0, now = 0;
	ResourceLimitType commit = RESLIMIT_COMMIT;
	if (R_FAILED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE))) svcBreak(USERBREAK_PANIC);
	svcGetResourceLimitLimitValues(&most, limit, &commit, 1);
	svcGetResourceLimitCurrentValues(&now, limit, &commit, 1);
	svcCloseHandle(limit);
	u32 remaining = (u32) (most - now) & ~0xFFFu;
	u32 area = OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN;
	u32 heap = remaining > 3 * LINEAR_WANTED ? remaining - LINEAR_WANTED : remaining / 2;
	if (heap > area) heap = area;
	u32 linear = remaining - heap;
	/* An emulator gives the 124 MiB a New 3DS's own software gets; the console
	 * gives homebrew 64 MiB (heap 41568 KiB, 2026-11-12). The same here, so
	 * that what is tested is what the console has. */
#ifdef GBADS_EMU_HEAP_KIB /* (a build option: in Azahar, the heap of a New 3DS console in its own mode, 84 MiB free at a job's start) */
	if (isEmulator() && heap > ((u32) GBADS_EMU_HEAP_KIB << 10)) heap = (u32) GBADS_EMU_HEAP_KIB << 10, linear = LINEAR_WANTED;
#else
	if (isEmulator() && heap > (41568u << 10)) heap = (41568u << 10) + (16u << 20) - LINEAR_WANTED, linear = LINEAR_WANTED;
#endif
	__ctru_heap_size = heap;
	__ctru_linear_heap_size = linear;
	if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, __ctru_heap_size, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE)))
		svcBreak(USERBREAK_PANIC);
	if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, MEMPERM_READ | MEMPERM_WRITE)))
		svcBreak(USERBREAK_PANIC);
	mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
	fake_heap_start = (char*) __ctru_heap;
	fake_heap_end = fake_heap_start + __ctru_heap_size;
}

void logLine(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

#ifdef GBADS_SLOW_SD
/* TEST ONLY (make SLOWSD=1): slow-sd.txt in the app's folder, "PERIOD STALL" in ms, freezes the card for the first STALL
 * ms of every PERIOD while a bench job plays (writes, opens and small reads wait). */
#include <reent.h>
static struct {
	u64 start;
	u32 period, stall;
	volatile bool armed;
	u32 gameThread;
} slowSd;
static u32 thread_id(void) {
	u32 id = 0;
	svcGetThreadId(&id, CUR_THREAD_HANDLE);
	return id;
}

static __thread bool inSlowHook;
static int slowSdSkipFd = -1; /* (a file that isn't on the card: the log's ring, once it is one) */

static void slow_sd_wait(const char* op, int fd, size_t n) {
	if (!slowSd.armed || inSlowHook || fd < 0 || fd == slowSdSkipFd) return;
	u64 into = (osGetTime() - slowSd.start) % slowSd.period;
	if (into >= slowSd.stall) return;
	inSlowHook = true;
	u64 t0 = osGetTime();
	svcSleepThread((s64) (slowSd.stall - into) * 1000000);
	logLine("slow sd: %s of %u bytes (fd %d) waited %u ms (%s thread)", op, (unsigned) n, fd, (unsigned) (osGetTime() - t0),
	        thread_id() == slowSd.gameThread ? "game" : "other");
	inSlowHook = false;
}

void host_slow_sd(bool on) {
	if (on && !slowSd.period) {
		FILE* f = fopen(APP_DIR "/slow-sd.txt", "r");
		unsigned period = 0, stall = 0;
		if (f && fscanf(f, "%u %u", &period, &stall) == 2 && period && stall < period) slowSd.period = period, slowSd.stall = stall;
		if (f) fclose(f);
		logLine("slow sd: %s", slowSd.period ? "on" : "no slow-sd.txt");
	}
	slowSd.gameThread = thread_id();
	slowSd.start = osGetTime();
	slowSd.armed = on && slowSd.period;
}

_ssize_t __real__write_r(struct _reent* r, int fd, const void* buf, size_t n);
_ssize_t __wrap__write_r(struct _reent* r, int fd, const void* buf, size_t n) {
	slow_sd_wait("write", fd, n);
	return __real__write_r(r, fd, buf, n);
}
_ssize_t __real__read_r(struct _reent* r, int fd, void* buf, size_t n);
_ssize_t __wrap__read_r(struct _reent* r, int fd, void* buf, size_t n) {
	if (n < 64 * 1024) slow_sd_wait("read", fd, n);
	return __real__read_r(r, fd, buf, n);
}
int __real__open_r(struct _reent* r, const char* path, int flags, int mode);
int __wrap__open_r(struct _reent* r, const char* path, int flags, int mode) {
	slow_sd_wait("open", 0, strlen(path));
	return __real__open_r(r, path, flags, mode);
}
#endif

/* The card's writer: the log (stderr, a device of its own, "logbuf:") and the bench's lines go through one ring in memory
 * to one thread, so a card that stops for seconds holds up neither. A full ring drops log writes and says how many; a bench
 * line with no room waits in a list, not in the game's frame (the writer shares core 3 with the hidden copy). */
enum { RING = 10 * 1024, REC_MAX = 1023, LINE_BYTES = 640, LINE_SPARE = 1300, REC_LINE = 0x8000, SPILL_MAX = 256 };
typedef struct Spilled {
	struct Spilled* next;
	u32 n;
	char text[];
} Spilled;
static struct {
	LightLock lock;
	LightEvent wake;
	char ring[RING];
	u32 head, used, dropped, lost, spilledNow;
	Spilled *spillHead, *spillTail;
	volatile bool threaded;
	bool quit;
	double worstMs;
	u64 lockWaitTicks; /* (the game thread's waits for the lock, since host_line_wait_ms last asked) */
	FILE* file;
	void (*writeLine)(const char* line);
	pthread_t thread;
} lg;

int host_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg);

static void ring_add(u32 flag, const char* data, u32 n) {
	u32 at = (lg.head + lg.used) % RING;
	u32 header = flag | n;
	char bytes[2] = { (char) (header & 0xFF), (char) (header >> 8) };
	for (u32 i = 0; i < 2; ++i) lg.ring[(at + i) % RING] = bytes[i];
	for (u32 i = 0; i < n; ++i) lg.ring[(at + 2 + i) % RING] = data[i];
	lg.used += 2 + n;
}

/* (the lock is held for a few instructions: a wait for it is counted, for the bench's stall line) */
static void ring_lock(void) {
	if (!LightLock_TryLock(&lg.lock)) return;
	u64 t0 = svcGetSystemTick();
	LightLock_Lock(&lg.lock);
	lg.lockWaitTicks += svcGetSystemTick() - t0;
}

static int logdev_open(struct _reent* r, void* file, const char* path, int flags, int mode) {
	(void) r, (void) file, (void) path, (void) flags, (void) mode;
	return 0;
}

static int logdev_close(struct _reent* r, void* file) {
	(void) r, (void) file;
	return 0;
}

/* The log's last bytes, kept apart from the ring, which drops writes while the card is busy: a bench job that fails to start
 * says why in its own line (host_log_tail), as the posted log may not reach that far. */
static char tail[600];
static u32 tailAt;

static void tail_add(const char* buf, size_t n) {
	if (n > sizeof(tail)) buf += n - sizeof(tail), n = sizeof(tail);
	for (size_t i = 0; i < n; ++i) tail[(tailAt + i) % sizeof(tail)] = buf[i];
	tailAt = (u32) ((tailAt + n) % sizeof(tail));
}

void host_log_tail(char* out, size_t size) {
	size_t k = 0;
	for (size_t i = 0; i < sizeof(tail) && k + 1 < size; ++i) {
		char c = tail[(tailAt + i) % sizeof(tail)];
		if (c) out[k++] = c == '\n' ? '|' : c;
	}
	out[k] = 0;
}

static ssize_t logdev_write(struct _reent* r, void* file, const char* buf, size_t n) {
	(void) r, (void) file;
	tail_add(buf, n);
	if (!lg.threaded) {
		if (lg.file && fwrite(buf, 1, n, lg.file) == n) fflush(lg.file);
		return (ssize_t) n;
	}
	for (size_t done = 0; done < n;) {
		u32 part = n - done < REC_MAX ? (u32) (n - done) : REC_MAX;
		ring_lock();
		if (lg.used + 2 + part + LINE_SPARE > RING) ++lg.dropped;
		else ring_add(0, buf + done, part);
		LightLock_Unlock(&lg.lock);
		done += part;
	}
	LightEvent_Signal(&lg.wake);
	return (ssize_t) n;
}

static const devoptab_t logDevice = {
	.name = "logbuf",
	.structSize = 4,
	.open_r = logdev_open,
	.close_r = logdev_close,
	.write_r = logdev_write,
};

/* (the lock is held) Lines the ring had no room for, in the order they came. */
/* (returns the moved nodes for the caller to free after unlocking: free waits on the malloc lock, which the hidden copy
 * holds while it compiles, and a writer stuck holding lg.lock behind it on core 3 stalls the game's next log write) */
static Spilled* spill_move(void) {
	Spilled* spent = NULL;
	while (lg.spillHead && lg.used + 2 + lg.spillHead->n <= RING) {
		Spilled* s = lg.spillHead;
		lg.spillHead = s->next;
		if (!lg.spillHead) lg.spillTail = NULL;
		--lg.spilledNow;
		ring_add(REC_LINE, s->text, s->n);
		s->next = spent;
		spent = s;
	}
	return spent;
}

static void spill_free(Spilled* s) {
	while (s) {
		Spilled* next = s->next;
		free(s);
		s = next;
	}
}

static void* writer_main(void* arg) {
	(void) arg;
	static char rec[REC_MAX + 1];
	for (;;) {
		LightLock_Lock(&lg.lock);
		Spilled* spent = spill_move();
		if (!lg.used) {
			bool quit = lg.quit;
			LightLock_Unlock(&lg.lock);
			spill_free(spent);
			if (quit) break;
			LightEvent_Wait(&lg.wake);
			continue;
		}
		u32 header = (unsigned char) lg.ring[lg.head] | (unsigned char) lg.ring[(lg.head + 1) % RING] << 8;
		u32 n = header & ~REC_LINE;
		for (u32 i = 0; i < n; ++i) rec[i] = lg.ring[(lg.head + 2 + i) % RING];
		rec[n] = 0;
		u32 dropped = lg.dropped, lost = lg.lost;
		lg.dropped = lg.lost = 0;
		LightLock_Unlock(&lg.lock);
		spill_free(spent);
		if (dropped) fprintf(lg.file, "\n[log: %u write(s) dropped, the SD card was busy]\n", (unsigned) dropped);
		if (lost && lg.file) fprintf(lg.file, "\n[bench: %u line(s) lost, the SD card was busy for minutes]\n", (unsigned) lost);
		if (header & REC_LINE) {
			u64 t0 = svcGetSystemTick();
			lg.writeLine(rec);
			if (lg.file && fprintf(lg.file, "%s\n", rec) > 0) fflush(lg.file);
			double ms = (double) (svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11;
			LightLock_Lock(&lg.lock);
			if (ms > lg.worstMs) lg.worstMs = ms;
			LightLock_Unlock(&lg.lock);
		} else if (fwrite(rec, 1, n, lg.file) == n) {
			fflush(lg.file);
		}
		LightLock_Lock(&lg.lock);
		lg.head = (lg.head + 2 + n) % RING;
		lg.used -= 2 + n;
		LightLock_Unlock(&lg.lock);
	}
	return NULL;
}

static bool linesFirst;
static int captureSteps;

void host_set_capture_priority(int steps) {
	captureSteps = steps < 0 ? 0 : steps > 8 ? 8 : steps;
}

/* The thread was made a step below the game; a thread that has the core keeps it until it waits, so every other core 3
 * thread of the same priority (the writers) has to wait out a whole capture. */
void host_capture_thread_lower(void) {
	if (!captureSteps || !isNew3ds()) return;
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	if (priority + captureSteps > 0x3F) priority = 0x3F - captureSteps;
	Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, priority + captureSteps);
	logLine("hidden copy: priority %d, %d step(s) lower (%08lX)", (int) (priority + captureSteps), captureSteps, (unsigned long) rc);
}

/* Above the hidden copy's thread: a writer that waits out its long blind stretches lets the ring fill, and the game then
 * waits for room. Core 3 also has the second render worker at this priority: read video wait. New 3DS only (the game's core). */
static void writer_priority(void) {
	if (!lg.threaded) return;
	s32 game = 0x30;
	svcGetThreadPriority(&game, CUR_THREAD_HANDLE);
	svcSetThreadPriority(threadGetHandle((Thread) lg.thread), linesFirst ? game : game + 1);
}

void host_set_lines_first(bool on) {
	on = on && isNew3ds();
	if (on == linesFirst) return;
	linesFirst = on;
	writer_priority();
	logLine("lines_first %s: the line writer's priority is the game's%s", on ? "on" : "off", on ? "" : " plus one");
}

static bool writer_start(const char* name, size_t stack) {
	if (lg.threaded) return true;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, stack);
	host_next_thread_name(name);
	lg.threaded = host_pthread_create(&lg.thread, &attr, writer_main, NULL) == 0;
	pthread_attr_destroy(&attr);
	writer_priority();
	return lg.threaded;
}

static void ring_drain(void) {
	while (lg.threaded && (lg.used || lg.spillHead)) {
		LightEvent_Signal(&lg.wake);
		svcSleepThread(1000000);
	}
}

static void writer_stop(void) {
	if (!lg.threaded) return;
	ring_drain();
	lg.quit = true;
	LightEvent_Signal(&lg.wake);
	pthread_join(lg.thread, NULL);
	lg.threaded = lg.quit = false;
}

/* Everything logged so far is in log.txt (the log is read, sent or ended after this). */
void host_log_flush(void) {
	fflush(stderr);
	ring_drain();
}

void host_line_put(const char* line, void (*write)(const char* line)) {
	lg.writeLine = write;
	u32 n = (u32) strnlen(line, LINE_BYTES - 1);
	if (!writer_start("bench lines", 32 * 1024)) {
		write(line);
		logLine("%s", line);
		return;
	}
	ring_lock();
	bool fits = !lg.spillHead && lg.used + 2 + n <= RING;
	if (fits) ring_add(REC_LINE, line, n);
	bool full = !fits && lg.spilledNow >= SPILL_MAX;
	if (full) ++lg.lost;
	LightLock_Unlock(&lg.lock);
	if (!fits && !full) {
		Spilled* s = malloc(sizeof(*s) + n + 1);
		ring_lock();
		if (s) {
			s->next = NULL;
			s->n = n;
			memcpy(s->text, line, n);
			s->text[n] = 0;
			if (lg.spillTail) lg.spillTail->next = s;
			else lg.spillHead = s;
			lg.spillTail = s;
			++lg.spilledNow;
		} else {
			++lg.lost;
		}
		LightLock_Unlock(&lg.lock);
	}
	LightEvent_Signal(&lg.wake);
}

void host_line_drain(void) {
	ring_drain();
}

void host_line_stop(void) {
	writer_stop();
}

double host_line_wait_ms(void) {
	LightLock_Lock(&lg.lock);
	double ms = (double) lg.lockWaitTicks * 1000.0 / SYSCLOCK_ARM11;
	lg.lockWaitTicks = 0;
	LightLock_Unlock(&lg.lock);
	return ms;
}

double host_line_worst_ms(void) {
	LightLock_Lock(&lg.lock);
	double ms = lg.worstMs;
	lg.worstMs = 0;
	LightLock_Unlock(&lg.lock);
	return ms;
}

static void log_start(void) {
	LightLock_Init(&lg.lock);
	LightEvent_Init(&lg.wake, RESET_ONESHOT);
	lg.file = fopen(APP_DIR "/log.txt", "w");
	if (!lg.file) return;
	if (AddDevice(&logDevice) < 0 || !freopen("logbuf:/log", "w", stderr)) {
		fclose(lg.file);
		lg.file = NULL;
		if (freopen(APP_DIR "/log.txt", "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 1024);
		return;
	}
	setvbuf(stderr, NULL, _IOLBF, 1024);
#ifdef GBADS_SLOW_SD
	slowSdSkipFd = fileno(stderr);
#endif
#ifndef GBADS_BENCH_TAG
	writer_start("log lines", 16 * 1024);
#endif
}

bool isEmulator(void) {
	static int known = -1;
	if (known < 0) {
		s64 value = 0;
		known = R_SUCCEEDED(svcGetSystemInfo(&value, 0x20000, 0)) && value != 0;
	}
	return known;
}

bool isNew3ds(void) {
	static int known = -1;
	if (known < 0) {
		bool isNew = false;
		APT_CheckNew3DS(&isNew);
		known = isNew;
	}
	return known;
}

size_t hostTopBytes; /* ROMs kept at the top of the heap's range (host_top_alloc), past malloc's reach */

/* ROMs at the top of the heap's address range, the heap's end brought down below them: the heap's
 * own allocations stay underneath, so a 32 MiB ROM finds one stretch however the heap is cut up. */
void __malloc_lock(struct _reent* r);
void __malloc_unlock(struct _reent* r);
enum { TOP_BLOCKS = 8, HEAP_ROOM = 4 << 20 /* (always left for the heap to grow into) */ };
static struct {
	uint8_t* at;
	size_t size;
} topBlocks[TOP_BLOCKS];
static char* topEnd; /* the heap's end before any ROM came */

/* (under the malloc lock: sbrk reads fake_heap_end) */
static void top_end_update(void) {
	char* low = topEnd;
	for (int i = 0; i < TOP_BLOCKS; ++i) {
		if (topBlocks[i].at && (char*) topBlocks[i].at < low) low = (char*) topBlocks[i].at;
	}
	fake_heap_end = low;
}

static uint8_t* top_alloc_room(size_t size, size_t room) {
	__malloc_lock(_REENT);
	if (!topEnd) topEnd = fake_heap_end;
	char* brk = sbrk(0);
	int slot = -1;
	for (int i = 0; i < TOP_BLOCKS && slot < 0; ++i) {
		if (!topBlocks[i].at) slot = i;
	}
	uint8_t* p = NULL;
	/* the highest gap that takes it: between the blocks, else below the lowest */
	for (char* ceiling = topEnd; slot >= 0;) {
		int below = -1;
		for (int i = 0; i < TOP_BLOCKS; ++i) {
			if (topBlocks[i].at && (char*) topBlocks[i].at < ceiling && (below < 0 || topBlocks[i].at > topBlocks[below].at)) below = i;
		}
		char* floor = below >= 0 ? (char*) topBlocks[below].at + topBlocks[below].size : brk + room;
		if (ceiling - floor >= (ptrdiff_t) size) {
			p = (uint8_t*) (ceiling - size);
			break;
		}
		if (below < 0) break;
		ceiling = (char*) topBlocks[below].at;
	}
	if (p) {
		topBlocks[slot].at = p;
		topBlocks[slot].size = size;
		hostTopBytes += size;
		top_end_update();
	}
	__malloc_unlock(_REENT);
	return p;
}

void* host_linear_alloc(size_t size) {
	return linearMemAlign(size, 0x80);
}

void host_linear_free(void* p) {
	linearFree(p);
}

uint8_t* host_top_alloc_code(size_t size) {
	return top_alloc_room((size + 0xFFF) & ~(size_t) 0xFFF, HEAP_ROOM);
}

uint8_t* host_top_alloc(size_t size) {
	size = (size + 0xFFF) & ~(size_t) 0xFFF;
	uint8_t* p = top_alloc_room(size, HEAP_ROOM);
	if (!p) {
		// A 32 MiB cartridge with armGBA's machine leaves about 1.5 MiB of the heap, so it gets less room
		// than the rest, once the heap's free end has been given back.
		malloc_trim(0);
		p = top_alloc_room(size, 1 << 20);
	}
	return p;
}

bool host_top_free(void* p) {
	bool found = false;
	if (!p) return false; /* (an empty slot's at is NULL too) */
	__malloc_lock(_REENT);
	for (int i = 0; i < TOP_BLOCKS && !found; ++i) {
		if (topBlocks[i].at != (uint8_t*) p) continue;
		hostTopBytes -= topBlocks[i].size;
		topBlocks[i].at = NULL;
		top_end_update();
		found = true;
	}
	__malloc_unlock(_REENT);
	return found;
}

bool host_top_owns(const void* p) {
	bool found = false;
	if (!p) return false;
	__malloc_lock(_REENT);
	for (int i = 0; i < TOP_BLOCKS && !found; ++i) found = topBlocks[i].at == (const uint8_t*) p;
	__malloc_unlock(_REENT);
	return found;
}

void host_rom_free(void* p) {
	if (p && !host_top_free(p)) free(p);
}


size_t host_heap_free(void) {
	struct mallinfo m = mallinfo();
	u32 used = (u32) m.uordblks + hostTopBytes;
	return __ctru_heap_size > used ? __ctru_heap_size - used : 0;
}

/* Walks malloc's chunks (newlib's: a size word at +4 with flags below 8, a chunk in use when the next one's PREV_INUSE bit
 * says so) to say where the heap's break sits and which live blocks hold it up. */
void host_heap_shape(char* out, size_t n) {
	enum { LIVE = 4 };
	struct {
		unsigned at, size;
	} high[LIVE] = { { 0, 0 } };
	unsigned base = (unsigned) fake_heap_start, brk = (unsigned) sbrk(0), p = base, hole = 0, ends, romBytes;
	__malloc_lock(_REENT);
	while (p + 16 <= brk) {
		unsigned size = *(unsigned*) (p + 4) & ~7u;
		if (size < 16 || p + size > brk) break;
		unsigned next = p + size;
		bool inUse = next + 8 <= brk && (*(unsigned*) (next + 4) & 1);
		if (inUse) {
			memmove(&high[1], &high[0], (LIVE - 1) * sizeof(high[0])); /* (the walk climbs: the newest is the highest) */
			high[0].at = p, high[0].size = size;
		} else if (next < brk - 64 && size > hole) {
			hole = size;
		}
		p = next;
	}
	ends = (unsigned) fake_heap_end, romBytes = (unsigned) hostTopBytes;
	__malloc_unlock(_REENT);
	int len = snprintf(out, n, "break at %u KiB, heap end at %u KiB, ROMs %u KiB, room for another ROM %u KiB, biggest hole %u KiB, highest live:",
	                   (brk - base) >> 10, (ends - base) >> 10, romBytes >> 10, brk + HEAP_ROOM < ends ? (ends - brk - HEAP_ROOM) >> 10 : 0, hole >> 10);
	for (int i = 0; i < LIVE && high[i].at && len > 0 && (size_t) len < n; ++i) {
		len += snprintf(out + len, n - len, " %uK+%u", (high[i].at - base) >> 10, high[i].size);
	}
	if (p + 16 <= brk && len > 0 && (size_t) len < n) snprintf(out + len, n - len, " (the walk stopped at %uK)", (p - base) >> 10);
}

/* Between a background thread's reads of a large file: a moment for the
 * screen's own thread to get its reads in, longer while the buttons are in use. */
static volatile u64 uiBusyUntil;
void host_ui_busy(void) {
	uiBusyUntil = osGetTime() + 1500;
}
void host_fs_yield(void) {
	/* (held off altogether while the buttons are in use: up to 1.5 s after the last press) */
	do svcSleepThread(osGetTime() < uiBusyUntil ? 20000000LL : 500000LL);
	while (osGetTime() < uiBusyUntil);
}

void host_mem_note(const char* what) {
	logLine("memory: %u KiB of the heap free, %u KiB of linear (%s)", (unsigned) (host_heap_free() >> 10), (unsigned) (linearSpaceFree() >> 10), what);
}

static int sideCore = -1, captureCore = -1, aheadCore = -1;
int gbadsVideoCore = 2; /* (the patched libmgba's threading.h reads it: mGBA's drawing thread) */
int gbadsVideoWake = 15; /* (and thread-proxy.c reads this one) */
static int core1Percent = 50;
static int core1Applied; /* (0: not yet: once set it can't go back, but another value can be asked for) */

/* A thread on the system core needs the app's share of it first; a later core1_limit (another job's) is asked for again. */
static void core_ready(int core) {
	if (core != 1) return;
	if (core1Percent < 5) core1Percent = 5;
	if (core1Percent > 89) core1Percent = 89;
	if (core1Applied == core1Percent) return;
	core1Applied = core1Percent;
	Result rc = APT_SetAppCpuTimeLimit(core1Percent);
	logLine("core 1: the app's share set to %d%% (%08lX)", core1Percent, (unsigned long) rc);
}

int host_side_core(void) {
	if (!isNew3ds()) return -2;
	int core = sideCore >= 0 ? sideCore : 3;
	core_ready(core);
	return core;
}

static int nextCore = HOST_CORE_AUTO;
void host_next_thread_core(int core) {
	nextCore = core;
}

int host_capture_core(void) {
	return captureCore >= 0 ? captureCore : HOST_CORE_AUTO;
}

int host_ahead_core(bool threadedVideo) {
	return aheadCore >= 0 ? aheadCore : threadedVideo ? 3 : 2;
}

struct spin {
	Thread thread;
	bool made;
	volatile u32 loops;
};

static void spin_main(void* p) {
	u64 end = svcGetSystemTick() + SYSCLOCK_ARM11 / 10;
	u32 loops = 0, x = 1;
	do {
		for (int i = 0; i < 2048; ++i) x = x * 1664525u + 1013904223u;
		++loops;
	} while (svcGetSystemTick() < end);
	((struct spin*) p)->loops = x ? loops : 0; /* (x is used, so the loop stays) */
}

static void spin_end(struct spin* s) {
	if (!s->thread) return;
	threadJoin(s->thread, U64_MAX);
	threadFree(s->thread);
	s->thread = NULL;
}

/* A tenth of a second of arithmetic on each core: the share of core 0's speed a thread there gets,
 * one core at a time and then all at once (the system's own threads take what they take). */
static void core_probe(void) {
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	struct spin ref = { 0 };
	spin_main(&ref);
	for (int together = 0; together < 2; ++together) {
		struct spin s[4] = { { 0 } };
		for (int core = 1; core < 4; ++core) {
			core_ready(core);
			host_next_thread_name("core probe");
			s[core].thread = threadCreate(spin_main, &s[core], 16 * 1024, priority, core, false);
			s[core].made = s[core].thread != NULL;
			if (!together) spin_end(&s[core]);
		}
		if (together) spin_main(&s[0]);
		char line[160];
		int n = snprintf(line, sizeof(line), "core probe, %s:", together ? "all at once" : "one at a time");
		for (int core = 1; core < 4; ++core) {
			spin_end(&s[core]);
			if (s[core].made) n += snprintf(line + n, sizeof(line) - n, " core %d %.2f", core, (double) s[core].loops / ref.loops);
			else n += snprintf(line + n, sizeof(line) - n, " core %d refused", core);
		}
		if (together) snprintf(line + n, sizeof(line) - n, " (core 0 %.2f)", (double) s[0].loops / ref.loops);
		logLine("%s", line);
	}
}

void host_set_video_wake(int mask) {
	gbadsVideoWake = mask == 0 || mask == 1 || mask == 3 || mask == 7 || mask == 15 || mask == 31 || mask == 63 ? mask : 15;
}

void host_set_cores(int side, int capture, int ahead, int video, int core1, bool probe) {
	sideCore = side;
	captureCore = capture;
	aheadCore = ahead;
	core1Percent = core1;
	gbadsVideoCore = video >= 0 && isNew3ds() ? video : 2;
	core_ready(gbadsVideoCore);
	logLine("cores: side %d, capture %d, ahead %d (-1: as built in), video %d, core 1 share %d%%", side, capture, ahead, gbadsVideoCore, core1);
	if (probe && isNew3ds()) core_probe();
}

/* Every thread made (libmgba's and libctru's too: the build wraps threadCreate), with the core it asked for and
 * the one svcGetProcessorID gave inside it. */
enum { THREAD_ROWS = 24 };
static struct {
	char name[48];
	s8 asked[4];
	u8 tries;
	int ran; /* -1: not started */
	bool made, alive;
} rows[THREAD_ROWS];
static LightLock rowsLock;
static const char* nextName;

static void __attribute__((constructor)) threads_init(void) {
	LightLock_Init(&rowsLock);
}

void host_next_thread_name(const char* name) {
	nextName = name;
}

struct traced {
	ThreadFunc fn;
	void* arg;
	int row;
};

static void traced_main(void* p) {
	struct traced t = *(struct traced*) p;
	free(p);
	int core = (int) svcGetProcessorID();
	LightLock_Lock(&rowsLock);
	rows[t.row].ran = core;
	rows[t.row].alive = true;
	LightLock_Unlock(&rowsLock);
	t.fn(t.arg);
	LightLock_Lock(&rowsLock);
	rows[t.row].alive = false;
	LightLock_Unlock(&rowsLock);
}

Thread __real_threadCreate(ThreadFunc entry, void* arg, size_t stackSize, int prio, int core, bool detached);
Thread __wrap_threadCreate(ThreadFunc entry, void* arg, size_t stackSize, int prio, int core, bool detached) {
	char name[sizeof(rows[0].name)];
	if (nextName) snprintf(name, sizeof(name), "%s", nextName);
	/* (known by their stack and priority: libmgba's threading.h, and libctru's aptInit, gspInit and ndspInit) */
	else if (stackSize == 0x8000 && prio == 0x18) snprintf(name, sizeof(name), "mgba video");
	else if (stackSize == 0x1000 && prio == 0x31) snprintf(name, sizeof(name), "apt events");
	else if (stackSize == 0x1000 && prio == 0x1a) snprintf(name, sizeof(name), "gpu events");
	else if (stackSize == 0x1000 && prio == 0x18) snprintf(name, sizeof(name), "audio (ndsp)");
	else snprintf(name, sizeof(name), "other %uK prio %x from %p", (unsigned) (stackSize >> 10), prio, __builtin_return_address(0));
	nextName = NULL;
	int row = -1;
	LightLock_Lock(&rowsLock);
	/* (a creator that failed on one core and tries another: the same row) */
	for (int i = 0; i < THREAD_ROWS && row < 0; ++i) {
		if (rows[i].tries && !rows[i].made && rows[i].tries < 4 && !strcmp(rows[i].name, name)) row = i;
	}
	for (int i = 0; i < THREAD_ROWS && row < 0; ++i) {
		if (!rows[i].tries) {
			row = i;
			memset(&rows[i], 0, sizeof(rows[i]));
			snprintf(rows[i].name, sizeof(rows[i].name), "%s", name);
			rows[i].ran = -1;
		}
	}
	if (row >= 0) rows[row].asked[rows[row].tries++] = (s8) core;
	LightLock_Unlock(&rowsLock);
	struct traced* t = row >= 0 ? malloc(sizeof(*t)) : NULL;
	if (t) *t = (struct traced) { entry, arg, row };
	Thread made = __real_threadCreate(t ? traced_main : entry, t ? (void*) t : arg, stackSize, prio, core, detached);
	if (!made) free(t);
	if (made && t) {
		LightLock_Lock(&rowsLock);
		rows[row].made = true;
		LightLock_Unlock(&rowsLock);
	}
	return made;
}

void host_threads_job_start(void) {
	LightLock_Lock(&rowsLock);
	for (int i = 0; i < THREAD_ROWS; ++i) {
		if (!rows[i].alive) rows[i].tries = 0;
	}
	LightLock_Unlock(&rowsLock);
}

/* (an emulator's processor id is not 0 to 3: marked with a ?) */
static const char* core_text(int core, char* buf) {
	snprintf(buf, 16, core >= 0 && core <= 3 ? "%d" : "?%d", core);
	return buf;
}

void host_threads_describe(char* out, size_t n) {
	char c[16];
	size_t used = (size_t) snprintf(out, n, "game core %s", core_text((int) svcGetProcessorID(), c));
	LightLock_Lock(&rowsLock);
	for (int i = 0; i < THREAD_ROWS && used < n; ++i) {
		if (!rows[i].tries) continue;
		used += (size_t) snprintf(out + used, n - used, "; %s ", rows[i].name);
		if (used >= n) break;
		if (!rows[i].made) used += (size_t) snprintf(out + used, n - used, "not made");
		else if (rows[i].ran < 0) used += (size_t) snprintf(out + used, n - used, "not started");
		else used += (size_t) snprintf(out + used, n - used, "core %s", core_text(rows[i].ran, c));
		if (rows[i].tries > 1 || rows[i].asked[0] != rows[i].ran) {
			for (int k = 0; k < rows[i].tries && used < n; ++k) {
				bool last = k == rows[i].tries - 1;
				used += (size_t) snprintf(out + used, n - used, "%s%d%s", k ? ", then " : " (asked ", rows[i].asked[k],
				                          last ? (rows[i].made ? ")" : ", refused)") : ", refused");
			}
		}
	}
	LightLock_Unlock(&rowsLock);
}

/* pthread_create for the app's threads (the build names it in place of the
 * C library's): on the side core, a step below the game, with room on the
 * stack for an emulator. libctru's pthread_t is its Thread, so pthread_join
 * works on these. */
struct start {
	void* (*fn)(void*);
	void* arg;
};

static void start_main(void* p) {
	struct start s = *(struct start*) p;
	free(p);
	logLine("thread %p: running on core %d", (void*) s.fn, (int) svcGetProcessorID());
	s.fn(s.arg);
}

int host_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
	/* (a thread that only writes lines asks for a small stack: armGBA with a 32 MiB game has under a MiB of heap) */
	size_t stack = 256 * 1024;
	if (attr && pthread_attr_getstacksize(attr, &stack) != 0) stack = 256 * 1024;
	if (stack < 16 * 1024) stack = 16 * 1024;
	struct start* s = malloc(sizeof(*s));
	if (!s) return ENOMEM;
	s->fn = fn;
	s->arg = arg;
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	int core = nextCore != HOST_CORE_AUTO && isNew3ds() ? nextCore : host_side_core();
	nextCore = HOST_CORE_AUTO;
	core_ready(core);
	const char* name = nextName ? nextName : "thread";
	nextName = name;
	Thread t = threadCreate(start_main, s, stack, priority + 1, core, false);
	if (!t && core != host_side_core()) {
		nextName = name;
		t = threadCreate(start_main, s, stack, priority + 1, core = host_side_core(), false);
	}
	if (!t && core != -2) {
		nextName = name;
		t = threadCreate(start_main, s, stack, priority + 1, core = -2, false);
	}
	logLine("thread %p: %s (core %d asked)", (void*) fn, t ? "made" : "not made", core);
	host_mem_note("a thread");
	if (!t) {
		free(s);
		return EAGAIN;
	}
	*thread = (pthread_t) t;
	return 0;
}

/* The SD card, opened once for the calls made on it here. */
static bool sd_archive(FS_Archive* archive) {
	static FS_Archive sd;
	static bool open;
	if (!open) open = R_SUCCEEDED(FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));
	*archive = sd;
	return open;
}

bool host_read_head(const char* path, void* out, size_t n) {
	if (!strncmp(path, "sdmc:", 5)) path += 5;
	FS_Archive archive;
	if (!sd_archive(&archive)) return false;
	Handle file;
	if (R_FAILED(FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0))) return false;
	u32 got = 0;
	Result rc = FSFILE_Read(file, &got, 0, out, (u32) n);
	FSFILE_Close(file);
	return R_SUCCEEDED(rc) && got == n;
}

int host_read_dir(const char* path, HostEntry** out) {
	*out = NULL;
	if (!strncmp(path, "sdmc:", 5)) path += 5;
	FS_Archive archive;
	if (!sd_archive(&archive)) return -1;
	Handle dir;
	if (R_FAILED(FSUSER_OpenDirectory(&dir, archive, fsMakePath(PATH_ASCII, path)))) return -1;
	enum { BATCH = 64 };
	FS_DirectoryEntry* batch = malloc(BATCH * sizeof(*batch));
	HostEntry* list = NULL;
	int count = 0;
	u32 got = 0;
	while (batch && R_SUCCEEDED(FSDIR_Read(dir, &got, BATCH, batch)) && got) {
		HostEntry* grown = realloc(list, (size_t) (count + (int) got) * sizeof(*list));
		if (!grown) break;
		list = grown;
		for (u32 i = 0; i < got; ++i) {
			HostEntry* e = &list[count++];
			ssize_t n = utf16_to_utf8((uint8_t*) e->name, batch[i].name, sizeof(e->name) - 1);
			e->name[n < 0 ? 0 : n >= (ssize_t) sizeof(e->name) ? (ssize_t) sizeof(e->name) - 1 : n] = 0;
			e->dir = (batch[i].attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
			e->size = (long long) batch[i].fileSize;
		}
	}
	free(batch);
	FSDIR_Close(dir);
	*out = list;
	return count;
}

/* stat as the C library has it leaves a file's time at 0: the SD card's own
 * record of it (the build links stat through here: -Wl,--wrap=stat). */
int __real_stat(const char* path, struct stat* st);
int __wrap_stat(const char* path, struct stat* st) {
	int rc = __real_stat(path, st);
	/* (not a game's: the game list reads a whole folder of them, and each
	 * question is another trip to the SD card) */
	const char* dot = strrchr(path, '.');
	bool game = dot && (!strcasecmp(dot, ".gba") || !strcasecmp(dot, ".zip"));
	if (rc == 0 && !game && !st->st_mtime && !S_ISDIR(st->st_mode)) {
		u64 mtime = 0;
		if (R_SUCCEEDED(archive_getmtime(path, &mtime))) st->st_mtime = (time_t) mtime;
	}
	return rc;
}

#ifdef GBADS_LOG_URL
/* A diagnosis build only (make LOG_URL=http://<host>:<port>/log, never a
 * release), as armGBA-3DS's and PokeDaisy's: a log POSTed to a machine on
 * the local network (3ds/tools/logserver.py), for finding out what happened
 * on a console without taking its SD card out. */
static void log_upload(const char* path, const char* name) {
	FILE* f = fopen(path, "rb");
	if (!f) return;
	fseek(f, 0, SEEK_END);
	long length = ftell(f);
	/* the end, if it's long: what happened last is what's wanted */
	long most = 768 * 1024;
	long from = length > most ? length - most : 0;
	size_t size = (size_t) (length - from);
	char* data = size ? malloc(size) : NULL;
	fseek(f, from, SEEK_SET);
	bool ok = data && fread(data, 1, size, f) == size;
	fclose(f);
	if (!ok) logLine("log upload: %s: nothing read of %s (%ld bytes)", name, path, length);
	/* (just started, the console may not be on the network yet: up to 5 s) */
	if (ok && R_SUCCEEDED(acInit())) {
		u32 wifi = 0;
		for (int i = 0; i < 50 && (R_FAILED(ACU_GetWifiStatus(&wifi)) || !wifi); ++i) svcSleepThread(100000000LL);
		if (!wifi) logLine("log upload: not on the network");
		acExit();
	}
	/* the http service holds a POST's body in memory shared with it */
	Result init = ok ? httpcInit(1 << 20) : 0;
	if (ok && R_FAILED(init)) logLine("log upload: %s: no http service (%08lX)", name, (unsigned long) init);
	if (ok && R_SUCCEEDED(init)) {
		char url[256];
		snprintf(url, sizeof(url), "%s?name=%s", GBADS_LOG_URL, name);
		httpcContext context;
		u32 status = 0;
		/* the body is sent while this waits, and closing the context cuts it off (the server saves it short): allow 4 KiB/s */
		u64 wait = 3000000000ull + (u64) size * 250000ull;
		if (wait > 120000000000ull) wait = 120000000000ull;
		Result rc = httpcOpenContext(&context, HTTPC_METHOD_POST, url, 0);
		if (R_SUCCEEDED(rc)) {
			httpcAddRequestHeaderField(&context, "Content-Type", "text/plain");
			httpcAddPostDataRaw(&context, (const u32*) data, size);
			rc = httpcBeginRequest(&context);
			if (R_SUCCEEDED(rc)) rc = httpcGetResponseStatusCodeTimeout(&context, &status, wait);
			httpcCloseContext(&context);
		}
		logLine("log upload: %s: %08lX, HTTP %lu", name, (unsigned long) rc, (unsigned long) status);
		httpcExit();
	}
	free(data);
}
#endif

#if defined(GBADS_LOG_URL) && defined(GBADS_BENCH_TAG)
/* The bench's jobs and scripts from the PC that runs the log server (logserver.py --serve DIR), into DIR on the SD
 * card; what's there already stays if the PC doesn't answer. */
static bool http_get(const char* url, const char* path) {
	httpcContext context;
	u32 status = 0, got = 0;
	bool ok = false;
	if (R_FAILED(httpcOpenContext(&context, HTTPC_METHOD_GET, url, 0))) return false;
	if (R_SUCCEEDED(httpcBeginRequest(&context)) && R_SUCCEEDED(httpcGetResponseStatusCodeTimeout(&context, &status, 5000000000ull)) &&
	    status == 200) {
		FILE* f = fopen(path, "wb");
		static u8 buf[16384];
		Result rc;
		do {
			rc = httpcDownloadData(&context, buf, sizeof(buf), &got);
			if (f && got) fwrite(buf, 1, got, f);
		} while (rc == (Result) HTTPC_RESULTCODE_DOWNLOADPENDING);
		ok = f && R_SUCCEEDED(rc);
		if (f) fclose(f);
	}
	httpcCloseContext(&context);
	logLine("bench download %s: %s (HTTP %lu)", url, ok ? "done" : "failed", (unsigned long) status);
	return ok;
}

static void bench_fetch(const char* dir) {
	char base[200], url[300], path[400], line[1024];
	snprintf(base, sizeof(base), "%s", GBADS_LOG_URL);
	char* slash = strrchr(base, '/');
	if (slash) *slash = 0; /* (http://host:port/log -> http://host:port) */
	if (R_FAILED(httpcInit(1 << 20))) return;
	snprintf(url, sizeof(url), "%s/bench/jobs.txt", base);
	snprintf(path, sizeof(path), "%s/jobs.txt", dir);
	if (http_get(url, path)) {
		FILE* f = fopen(path, "r");
		while (f && fgets(line, sizeof(line), f)) {
			/* job ID | ROM | SCRIPT | FRAMES: the script */
			char* p = line;
			for (int i = 0; i < 2 && p; ++i) p = strchr(p + 1, '|');
			if (strncmp(line, "job ", 4) || !p) continue;
			char script[200];
			if (sscanf(p + 1, " %199[^|\n ]", script) != 1) continue;
			snprintf(url, sizeof(url), "%s/bench/%s", base, script);
			char out[600];
			snprintf(out, sizeof(out), "%s/%s", dir, script);
			http_get(url, out);
		}
		if (f) fclose(f);
	}
	httpcExit();
}

void host_bench_report(const char* path, const char* what) {
	char name[160];
	snprintf(name, sizeof(name), "bench-%s-%s", GBADS_BENCH_TAG, what);
	log_upload(path, name);
}
#else
void host_bench_report(const char* path, const char* what) {
	(void) path, (void) what;
}
#endif

/* The log so far, sent now: the game list calls this each time it comes up
 * (at start, and back from a game that ran or didn't), so what happened is
 * here without the app having to end well. */
void host_log_checkpoint(void) {
#ifdef GBADS_LOG_URL
	host_log_flush();
	log_upload(APP_DIR "/log.txt", "gba-ds-now");
#endif
}

int gbads_main(int argc, char** argv);

int main(void) {
	osSetSpeedupEnable(true);
	mkdir("/3ds", 0777);
	mkdir(APP_DIR, 0777);
	mkdir("/roms", 0777);
	mkdir(ROM_DIR, 0777);
	/* the last run's log kept: what to attach to a bug report */
	remove(APP_DIR "/log-prev.txt");
	rename(APP_DIR "/log.txt", APP_DIR "/log-prev.txt");
	log_start();
	/* the 3DS's own defaults, in a file to change them in: where the games
	 * are; each picture as large as its screen takes; no rewind (the memory
	 * is the game's and the recompiler's) */
	FILE* f = fopen(APP_DIR "/gba-ds.cfg", "r");
	if (f) {
		fclose(f);
	} else if ((f = fopen(APP_DIR "/gba-ds.cfg", "w"))) {
		fputs("# GBA DS on the 3DS\nrom_dir = " ROM_DIR "\nscale = fit\nmap_scale = fit\nrewind = 0\n", f);
		fclose(f);
	}
	setenv("GBADS_HOME", APP_DIR, 1);
#ifdef GBADS_LOG_URL
	setenv("GBADS_DEBUG", "1", 1); /* capture times and the like, for the diagnosis */
#endif
	logLine("GBA DS for the 3DS %s (GBA DS %s)%s", GBADS_VERSION, GBADS_UPSTREAM,
#ifdef GBADS_LOG_URL
	        ", diagnosis build: logs to " GBADS_LOG_URL
#else
	        ""
#endif
	);
	logLine("%s 3DS%s, heap %lu KiB, linear %lu KiB", isNew3ds() ? "New" : "Old", isEmulator() ? " (emulator)" : "",
	        (unsigned long) (__ctru_heap_size >> 10), (unsigned long) (__ctru_linear_heap_size >> 10));
	char name[] = "gba-ds";
	char* args[] = { name, NULL, NULL, NULL };
	int nArgs = 1;
#ifdef GBADS_LOG_URL
	log_upload(APP_DIR "/log-prev.txt", "gba-ds-prev"); /* the last run's: all of it if that one crashed */
#endif
#ifdef GBADS_BENCH_TAG
	/* a bench build: its jobs (from the PC, with LOG_URL; else as they are on the SD card), then nothing but them */
	static char benchArg[200];
	mkdir(BENCH_DIR, 0777);
#ifdef GBADS_LOG_URL
	bench_fetch(BENCH_DIR);
#endif
	snprintf(benchArg, sizeof(benchArg), "--bench=%s/jobs.txt", BENCH_DIR);
	args[nArgs++] = benchArg;
	/* (the bench's own copy of the games, if it's there: beside nobody's saves, whatever gba-ds.cfg says) */
	static char romArg[] = "--rom_dir=" BENCH_ROMS;
	struct stat romSt;
	if (stat(BENCH_ROMS, &romSt) == 0 && S_ISDIR(romSt.st_mode)) args[nArgs++] = romArg;
#endif
	int rc = gbads_main(nArgs, args);
#ifdef GBADS_ARMGBA
	jitHostRestoreMemory();
#endif
	host_log_flush();
#ifdef GBADS_LOG_URL
	log_upload(APP_DIR "/log.txt", "gba-ds");
#endif
	writer_stop();
	return rc;
}

/* (a diagnosis, linked in by the test build's --wrap: who takes the heap) */
void* __real_malloc(size_t n);
void* __real_calloc(size_t a, size_t b);
void* __wrap_malloc(size_t n) {
	if (n >= 48 * 1024) logLine("malloc %u KiB from %p", (unsigned) (n >> 10), __builtin_return_address(0));
	return __real_malloc(n);
}
void* __wrap_calloc(size_t a, size_t b) {
	if (a * b >= 48 * 1024) logLine("calloc %u KiB from %p", (unsigned) ((a * b) >> 10), __builtin_return_address(0));
	return __real_calloc(a, b);
}
