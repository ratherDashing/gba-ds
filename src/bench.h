/* The test bench (bench = JOBS): every job starts a game from power-on, plays its input script unpaced and logs
 * frame times every 60 frames (after "second screen": video wait, hidden copy, hidden frames and, on the 3DS, gpu
 * wait; then, on the 3DS, pre, gap and sd: the "other" time's parts and the longest write, and a frame over 1 s gets a
 * "stall at frame" line). A 3DS job also logs which core each thread ran on, and a job line's OPTIONS (bench.c) set gba-ds.cfg keys. */
#ifndef GBADS_BENCH_H
#define GBADS_BENCH_H

#include "config.h"

#include <stdbool.h>
#include <stdint.h>

/* Runs the jobs file cfg->bench; false if it can't be read. */
bool bench_run(Config* cfg);
bool bench_active(void);
/* A line of the running job's own ("bench JOB " and then text, under 600 bytes), written with its results and never dropped. */
void bench_note(const char* text);
/* Once a frame from the game loop: this frame's emulating and drawing times, and GBA DS's own work between them
 * (the second screen's map, HUD and snapshot pictures); the loop's work before emulating, and the whole of its
 * body (what the frame took outside it is the lines written, the loop's start and its end). */
void bench_frame(uint64_t frame, double emuMs, double drawMs, double screenMs, bool captureBusy, double preMs, double loopMs);
/* After bench_frame: with bench_check = N, every N frames a "check" line with checksums of the picture, RAM and video
 * memory (two builds that print the same lines ran the same game the same way). */
struct Emu;
void bench_check(struct Emu* e, uint64_t frame);
/* A frame that spent 100 ms or more before emulating: a line saying where in the top of the loop it went. */
void bench_pre_note(uint64_t frame, double preMs, double inputMs, double midMs, double healthMs, double restMs);

#endif
