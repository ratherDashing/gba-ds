/* The 3DS's part of src/emu.c: one copy of a ROM in memory for every copy of the game playing it
 * (the game, the hidden copy, a logo's), read off the SD card in pieces. mGBA copies it only for a
 * copy that changes it (a profile's patch, a cartridge's clock or rumble). */
#include "host.h"

#include <mgba/core/core.h>
#include <mgba-util/vfs.h>

#include <3ds.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

uint8_t* emu_rom_read_all(const char* path, size_t* size);
bool emu_is_archive_name(const char* path);

typedef struct {
	char path[1024];
	uint8_t* rom;
	size_t size;
	bool top; /* at the top of the heap's range (host_top_alloc), else malloc'd */
	int users;
} SharedRom;

enum { MAX_ROMS = 4, MAX_CORES = 8 };
static SharedRom roms[MAX_ROMS];
static struct {
	const struct mCore* core;
	int rom;
} holders[MAX_CORES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* (mGBA's config code makes its folder through this; mGBA's own 3DS app opens it at start) */
FS_Archive sdmcArchive;

/* A background thread's reading of a ROM can be given up part-way (a logo being made when a game
 * is started): the flag it set for itself. */
static _Thread_local const volatile bool* readCancel;
void emu_read_cancel(const volatile bool* flag) {
	readCancel = flag;
}

/* mGBA's own (memory-map-hook.patch): a MiB or more (a ROM copy for a copy of the game that writes
 * to it, an odd-sized ROM's 32 MiB) from the top as well, the rest from the heap. */
void* anonymousMemoryMap(size_t size) {
	if (size >= 1 << 20) {
		uint8_t* p = host_top_alloc(size);
		if (p) {
			memset(p, 0, size);
			return p;
		}
	}
	return calloc(1, size);
}

void mappedMemoryFree(void* memory, size_t size) {
	(void) size;
	if (memory && !host_top_free(memory)) free(memory);
}

static void rom_free(uint8_t* p, bool top) {
	if (top) host_top_free(p);
	else free(p);
}

static uint8_t* read_rom(const char* path, size_t* n, bool* top) {
	*top = false;
	if (emu_is_archive_name(path)) return emu_rom_read_all(path, n);
	struct stat st;
	*n = 0;
	if (stat(path, &st) || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) return NULL;
	FILE* f = fopen(path, "rb");
	uint8_t* p = f ? host_top_alloc((size_t) st.st_size) : NULL;
	if (p) *top = true;
	else if (f) p = malloc((size_t) st.st_size);
	if (!f) fprintf(stderr, "%s: can't be opened\n", path);
	else if (!p) {
		char shape[400];
		host_heap_shape(shape, sizeof(shape));
		fprintf(stderr, "%s: no memory for its %u KiB (%u KiB of the heap free): %s\n", path, (unsigned) (st.st_size >> 10), (unsigned) (host_heap_free() >> 10), shape);
	}
	/* (in pieces from a background thread: one read of a whole ROM holds the SD card for seconds,
	 * and the game list's own reads wait behind it) */
	bool side = threadGetCurrent() != NULL;
	size_t got = 0;
	while (p && got < (size_t) st.st_size) {
		size_t want = (size_t) st.st_size - got;
		if (side && want > 64 * 1024) want = 64 * 1024;
		if (readCancel && *readCancel) break;
		size_t r = fread(p + got, 1, want, f);
		if (!r) {
			fprintf(stderr, "%s: read stopped at %u of %u bytes\n", path, (unsigned) got, (unsigned) st.st_size);
			break;
		}
		got += r;
		if (side) host_fs_yield();
	}
	if (f) fclose(f);
	if (p && got == (size_t) st.st_size) {
		*n = got;
		return p;
	}
	if (p) rom_free(p, *top);
	return NULL;
}

/* (under the lock) */
static int find_rom(const char* path) {
	for (int i = 0; i < MAX_ROMS; ++i) {
		if (roms[i].rom && !strcmp(roms[i].path, path)) return i;
	}
	return -1;
}

static void drop_if_unused(int slot) {
	if (slot >= 0 && !roms[slot].users) {
		rom_free(roms[slot].rom, roms[slot].top);
		roms[slot].rom = NULL;
	}
}

bool emu_ctr_load(struct mCore* core, const char* path) {
	pthread_mutex_lock(&lock);
	if (!sdmcArchive) FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
	int slot = find_rom(path);
	if (slot < 0) {
		/* (read outside the lock: another copy's start needn't wait for this one's SD card) */
		pthread_mutex_unlock(&lock);
		size_t size;
		bool top;
		uint8_t* rom = read_rom(path, &size, &top);
		pthread_mutex_lock(&lock);
		if (!rom) {
			pthread_mutex_unlock(&lock);
			return false;
		}
		slot = find_rom(path);
		if (slot >= 0) {
			rom_free(rom, top); /* (read twice at once: the first one kept) */
		} else {
			for (int i = 0; i < MAX_ROMS && slot < 0; ++i) {
				if (!roms[i].rom) slot = i;
			}
			if (slot >= 0) {
				snprintf(roms[slot].path, sizeof(roms[slot].path), "%s", path);
				roms[slot].rom = rom;
				roms[slot].size = size;
				roms[slot].top = top;
				roms[slot].users = 0;
			} else {
				rom_free(rom, top);
			}
		}
	}
	int holder = -1;
	for (int i = 0; i < MAX_CORES && holder < 0; ++i) {
		if (!holders[i].core) holder = i;
	}
	if (slot < 0 || holder < 0) {
		drop_if_unused(slot);
		pthread_mutex_unlock(&lock);
		fprintf(stderr, "%s: too many copies open to share its ROM\n", path);
		return false;
	}
	struct VFile* vf = VFileFromConstMemory(roms[slot].rom, roms[slot].size);
	core->unloadROM(core);
	bool ok = vf && core->loadROM(core, vf);
	if (ok) {
		++roms[slot].users;
		holders[holder].core = core;
		holders[holder].rom = slot;
	} else if (vf) {
		vf->close(vf);
	}
	drop_if_unused(slot);
	pthread_mutex_unlock(&lock);
	return ok;
}

/* After the core is gone (mGBA keeps reading the ROM until then). */
void emu_ctr_unload(const struct mCore* core) {
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_CORES; ++i) {
		if (holders[i].core != core) continue;
		SharedRom* r = &roms[holders[i].rom];
		holders[i].core = NULL;
		if (--r->users <= 0) {
			rom_free(r->rom, r->top);
			r->rom = NULL;
			r->users = 0;
		}
	}
	pthread_mutex_unlock(&lock);
}
