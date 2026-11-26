/* What the 3DS itself gives GBA DS besides SDL's part (sdl_ctr.c): its log,
 * its model, its memory, its cores - and what armGBA asks of a 3DS host
 * (jitmem.c). */
#ifndef GBADS_CTR_HOST_H
#define GBADS_CTR_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* armGBA's names for them (its 3DS host code calls these). */
void logLine(const char* format, ...) __attribute__((format(printf, 1, 2)));
bool isEmulator(void); /* Azahar and Citra */
bool isNew3ds(void);

size_t host_heap_free(void);
size_t host_heap_floor(void);
/* One line on where malloc's break sits and the live blocks just under it (the bench). */
void host_heap_shape(char* out, size_t n);
/* A ROM at the top of the heap's range, out of the way of the heap's own allocations (NULL: no room). */
uint8_t* host_top_alloc(size_t size);
/* The game's code memory at the top as well, under its ROM, if the heap's break leaves it room (NULL: it doesn't; given back with
 * host_rom_free). Made in the heap, its peak takes the break up with it, and what the game leaves behind there holds it. */
uint8_t* host_top_alloc_code(size_t size);
bool host_top_free(void* p); /* false: not one of those */
bool host_top_owns(const void* p);
void host_rom_free(void* p); /* either kind */
/* Memory the GPU can read (the game's picture), 128-byte aligned. */
void* host_linear_alloc(size_t size);
void host_linear_free(void* p);
/* The core the app's side work runs on (the hidden copy of the game that
 * draws the second screen, a logo's extraction, files): the New 3DS's
 * fourth (side_core in gba-ds.cfg), else whichever the system gives. */
int host_side_core(void);
/* The core the next pthread_create puts its thread on (the New 3DS's third, 2, for the game's own
 * frames), once; the side core after. */
#define HOST_CORE_AUTO (-100)
void host_next_thread_core(int core);
/* The calling thread's core, and the instruction cache of that core emptied (each core has its own, and code another core
 * wrote is what it may still hold from before: armgba_icache=0 turns the emptying off, to compare; false then). */
int host_core_now(void);
bool host_icache_invalidate(void);
/* The name the next thread made is listed under (host_threads_describe), once. */
void host_next_thread_name(const char* name);
/* The bench: a job begins (ended threads are forgotten) / one line with the core each thread found itself on and
 * whether the core asked for was refused. */
void host_threads_job_start(void);
/* The log's last few hundred bytes, newlines as '|' (for a bench job that failed to start). */
void host_log_tail(char* out, size_t size);
void host_threads_describe(char* out, size_t n);
/* The time sdl_ctr.c's drawing has waited for the GPU, in SDL_GetPerformanceCounter ticks (a sum that wraps). */
uint32_t host_gpu_wait_ticks(void);
/* The textures let go so far, freed now rather than when the next frame begins (sdl_ctr.c). */
void host_textures_free_now(void);
/* gba-ds.cfg's side_core, capture_core, ahead_core, video_core (-1: as built in), core1_limit (a thread on
 * core 1 needs the app's share of it set) and core_probe; at the start, and again for each game. */
void host_set_cores(int side, int capture, int ahead, int video, int core1Percent, bool probe);
/* gba-ds.cfg's video_wake: mGBA's drawing thread is woken when a line number's low bits (this mask: 0, 1, 3, 7, 15, 31, 63) are all set. */
void host_set_video_wake(int mask);
/* gba-ds.cfg's cpu_tile: the CPU tiles a whole 240x160 picture (once its self-test against the GPU's tiling passes). */
void host_set_cpu_tile(bool on);
/* gba-ds.cfg's async_upload: a picture's copy to its texture runs on the GPU while the next frame emulates. */
void host_set_async_upload(bool on);
/* gba-ds.cfg's gpu_twin: the render worker also writes each frame in the GPU's byte order (rows HOST_TWIN_STRIDE words apart),
 * which the copy engine reads where it is. host_gpu_twin_ready: on, and the start's self-test passed. host_picture_twin(fb, twin)
 * says the pixels fb have that twin (NULL: none) for the texture updates that follow. host_flush_data: write back the cache. */
#define HOST_TWIN_STRIDE 256 /* (= emu.h's EMU_TWIN_STRIDE) */
void host_set_gpu_twin(bool on);
void host_set_twin_check(bool on); /* gba-ds.cfg's twin_check (a test): each twin is compared with its picture before use */
void host_set_tex_check(bool on);  /* gba-ds.cfg's tex_check (a test): each picture texture is read back after the GPU's copy and compared */
bool host_gpu_twin_ready(void);
void host_picture_twin(const void* fb, const void* gpuOrder);
/* gba-ds.cfg's raw_picture: the render worker's picture buffers are linear memory it flushes itself, so the copy engine tiles them as they are and the
 * GPU's combiner puts the channels right. host_raw_picture_ready: on, and the self-test passed; host_picture_raw(fb): fb is such a buffer (NULL: none). */
#define HOST_RAW_SLACK_WORDS 64 /* (room after a picture buffer, in case the copy engine reads a little past its last row) */
void host_set_raw_picture(int mode); /* 0 off; 1 the picture buffers' rows 256 words apart, one transfer; 2 rows 240 apart, two passes (a linear copy with gaps, then the transfer) */
int host_raw_picture_pitch(void);    /* the words a row of the picture buffers has in this mode */
void host_set_raw_check(bool on); /* gba-ds.cfg's raw_check (a test): each raw picture is also drawn the old way, offscreen, and compared */
bool host_raw_picture_ready(void);
void host_picture_raw(const void* fb);
/* At a game's end: what tex_check and raw_check found, in the log and in the bench's results (a long job's log can be cut). */
void host_checks_report(void);
void host_flush_data(const void* p, size_t n);
/* The system tick (svcGetSystemTick) and its ticks a millisecond. */
uint64_t host_ticks(void);
#define HOST_TICKS_PER_MS 268111.856
/* Waits until the GPU has done what is queued (a buffer it was to read may be freed after). */
void host_gpu_settle(void);
/* What to give host_next_thread_core for the map capture's thread, and for the Frame Buffer's (mGBA's
 * drawing thread on or not). */
int host_capture_core(void);
int host_ahead_core(bool threadedVideo);
/* The next machine made is the game itself (the large share of the
 * recompiler's memory), or a copy. */
void host_log_checkpoint(void);
/* The log (stderr) is written to log.txt on a thread of its own; this waits until it all is (not for play). */
void host_log_flush(void);
/* The bench's lines go through the same thread: put one (write is called with it, on the thread), wait for all that
 * were put, stop the thread, and the longest write in ms since the last call. */
void host_line_put(const char* line, void (*write)(const char* line));
/* lines_first: the writer thread at the game's priority instead of a step below it (above the hidden copy's thread). */
void host_set_lines_first(bool on);
/* capture_priority: the hidden copy's thread this many steps below the other app threads (3DS), so the line writer, the
 * save writer and the like run as soon as they're ready; called by the thread itself at its start. */
void host_set_capture_priority(int steps);
void host_capture_thread_lower(void);
void host_line_drain(void);
void host_line_stop(void);
double host_line_worst_ms(void);
double host_line_wait_ms(void);
void host_ui_busy(void);
void host_fs_yield(void);
void host_mem_note(const char* what);
void host_jit_main(bool main, size_t romToLoad, bool hiddenCopy);
/* The game's ROM size while it plays (0 once it's closed): armgba_copy_code_mib's default depends on it. */
void host_jit_game_rom(size_t bytes);
/* A folder on the SD card listed in one go: its entries (malloc'd, the
 * caller frees them), each with whether it's a folder and a file's size.
 * Returns how many, or -1 if the folder can't be read. */
typedef struct {
	char name[256];
	bool dir;
	long long size;
} HostEntry;
int host_read_dir(const char* path, HostEntry** out);
/* The first n bytes of a file on the SD card, with as few calls on it as
 * that takes. */
bool host_read_head(const char* path, void* out, size_t n);
/* A bench build: a job's results to the PC (a LOG_URL build) as bench-TAG-WHAT. */
void host_bench_report(const char* path, const char* what);
/* TEST ONLY (a SLOWSD=1 build): the SD card freezes on a schedule (slow-sd.txt) while on. */
void host_slow_sd(bool on);
/* Before the app exits. */
void jitHostRestoreMemory(void);

#endif
