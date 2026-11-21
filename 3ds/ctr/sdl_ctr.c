/* sdl/SDL.h for the Nintendo 3DS: the part of SDL2 that GBA DS uses, drawn
 * with the GPU (citro2d), the buttons and the touch screen read through
 * hid, sound through ndsp. The top screen is display 0 (400x240), the
 * bottom one display 1 (320x240); each window fills its display.
 *
 * One GPU frame holds both screens: it begins at the first drawing call
 * after the last one ended, and ends when every window's renderer has been
 * presented. */
#include "sdl/SDL.h"

#include "host.h"
#include "bench.h"

#include <3ds.h>
#include <stdatomic.h>
#include <citro2d.h>

#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

/* ------------------------------------------------------------ the basics */

static char sdlError[160] = "";
static u64 startTick;

static void set_error(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(sdlError, sizeof(sdlError), fmt, ap);
	va_end(ap);
}

const char* SDL_GetError(void) {
	return sdlError;
}

const char* SDL_GetPlatform(void) {
	return "Nintendo 3DS";
}

SDL_bool SDL_SetHint(const char* name, const char* value) {
	(void) name;
	(void) value;
	return SDL_TRUE;
}

int SDL_ShowCursor(int toggle) {
	(void) toggle;
	return 0;
}

/* The system tick, not osGetTime: that clock steps back, which the game loop took for a pause of over 2 s (and rebuilt the pictures). */
static u64 mono_ms(void) {
	if (!startTick) startTick = svcGetSystemTick();
	return (svcGetSystemTick() - startTick) * 1000 / SYSCLOCK_ARM11;
}

Uint32 SDL_GetTicks(void) {
	return (Uint32) mono_ms();
}

Uint64 SDL_GetPerformanceCounter(void) {
	return svcGetSystemTick();
}

Uint64 SDL_GetPerformanceFrequency(void) {
	return SYSCLOCK_ARM11;
}

void SDL_Delay(Uint32 ms) {
	svcSleepThread((s64) ms * 1000000LL);
}

SDL_PowerState SDL_GetPowerInfo(int* seconds, int* percent) {
	if (seconds) *seconds = -1;
	u8 level = 0, charging = 0;
	if (R_FAILED(PTMU_GetBatteryLevel(&level))) {
		if (percent) *percent = -1;
		return SDL_POWERSTATE_UNKNOWN;
	}
	PTMU_GetBatteryChargeState(&charging);
	/* (the system gives a level from 0 to 5) */
	if (percent) *percent = level >= 5 ? 100 : level * 20;
	return charging ? SDL_POWERSTATE_CHARGING : SDL_POWERSTATE_ON_BATTERY;
}

SDL_bool SDL_IntersectRect(const SDL_Rect* a, const SDL_Rect* b, SDL_Rect* out) {
	int x0 = SDL_max(a->x, b->x), y0 = SDL_max(a->y, b->y);
	int x1 = SDL_min(a->x + a->w, b->x + b->w), y1 = SDL_min(a->y + a->h, b->y + b->h);
	out->x = x0, out->y = y0;
	out->w = x1 > x0 ? x1 - x0 : 0;
	out->h = y1 > y0 ? y1 - y0 : 0;
	return out->w && out->h ? SDL_TRUE : SDL_FALSE;
}

/* ------------------------------------------------------------ threads */

struct SDL_Thread {
	Thread t;
	SDL_ThreadFunction fn;
	void* data;
};
struct SDL_mutex {
	RecursiveLock lock;
};

static void thread_main(void* arg) {
	SDL_Thread* t = arg;
	t->fn(t->data);
}

/* The app's own side threads (a save written, a file read): on the system's
 * core, below the game. */
SDL_Thread* SDL_CreateThread(SDL_ThreadFunction fn, const char* name, void* data) {
	SDL_Thread* t = calloc(1, sizeof(*t));
	if (!t) return NULL;
	t->fn = fn;
	t->data = data;
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	host_next_thread_name(name ? name : "sdl thread");
	t->t = threadCreate(thread_main, t, 64 * 1024, priority + 1, host_side_core(), true);
	if (!t->t) {
		set_error("no thread");
		free(t);
		return NULL;
	}
	return t;
}

void SDL_DetachThread(SDL_Thread* thread) {
	(void) thread; /* (made detached: it frees itself; the small SDL_Thread is left) */
}

SDL_mutex* SDL_CreateMutex(void) {
	SDL_mutex* m = calloc(1, sizeof(*m));
	if (m) RecursiveLock_Init(&m->lock);
	return m;
}

int SDL_LockMutex(SDL_mutex* m) {
	if (m) RecursiveLock_Lock(&m->lock);
	return 0;
}

int SDL_UnlockMutex(SDL_mutex* m) {
	if (m) RecursiveLock_Unlock(&m->lock);
	return 0;
}

/* ------------------------------------------------------------ surfaces */

SDL_Surface* SDL_CreateRGBSurfaceWithFormatFrom(void* pixels, int w, int h, int depth, int pitch, Uint32 format) {
	(void) depth;
	SDL_Surface* s = calloc(1, sizeof(*s));
	if (!s) return NULL;
	s->w = w, s->h = h, s->pitch = pitch;
	s->pixels = pixels;
	s->format = &s->formatStore;
	s->formatStore.format = format;
	s->formatStore.BitsPerPixel = 32;
	s->formatStore.BytesPerPixel = 4;
	return s;
}

SDL_Surface* SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int w, int h, int depth, Uint32 format) {
	(void) flags;
	void* px = calloc((size_t) SDL_max(w, 1) * SDL_max(h, 1), 4);
	if (!px) {
		set_error("no memory for a %dx%d picture", w, h);
		return NULL;
	}
	SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(px, w, h, depth, w * 4, format);
	if (!s) {
		free(px);
		return NULL;
	}
	s->ownsPixels = true;
	return s;
}

void SDL_FreeSurface(SDL_Surface* s) {
	if (!s) return;
	if (s->ownsPixels) free(s->pixels);
	free(s);
}

SDL_Surface* SDL_DuplicateSurface(SDL_Surface* s) {
	if (!s) return NULL;
	SDL_Surface* d = SDL_CreateRGBSurfaceWithFormat(0, s->w, s->h, 32, s->format->format);
	if (!d) return NULL;
	for (int y = 0; y < s->h; ++y) memcpy((Uint8*) d->pixels + y * d->pitch, (const Uint8*) s->pixels + y * s->pitch, (size_t) s->w * 4);
	return d;
}

Uint32 SDL_MapRGBA(const SDL_PixelFormat* f, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
	if (f->format == SDL_PIXELFORMAT_ARGB8888) return (Uint32) a << 24 | (Uint32) r << 16 | (Uint32) g << 8 | b;
	return (Uint32) a << 24 | (Uint32) b << 16 | (Uint32) g << 8 | r;
}

static void put16(FILE* f, unsigned v) {
	fputc(v & 0xFF, f), fputc(v >> 8 & 0xFF, f);
}
static void put32(FILE* f, unsigned v) {
	put16(f, v & 0xFFFF), put16(f, v >> 16);
}

/* 24-bit BMPs: the thumbnails a save state keeps. */
int SDL_SaveBMP(SDL_Surface* s, const char* path) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		set_error("can't write %s", path);
		return -1;
	}
	int row = (s->w * 3 + 3) & ~3;
	fputc('B', f), fputc('M', f);
	put32(f, 54 + row * s->h), put32(f, 0), put32(f, 54);
	put32(f, 40), put32(f, s->w), put32(f, s->h), put16(f, 1), put16(f, 24);
	put32(f, 0), put32(f, row * s->h), put32(f, 2835), put32(f, 2835), put32(f, 0), put32(f, 0);
	Uint8* line = calloc(1, row);
	bool argb = s->format->format == SDL_PIXELFORMAT_ARGB8888;
	for (int y = s->h - 1; line && y >= 0; --y) {
		const Uint32* src = (const Uint32*) ((const Uint8*) s->pixels + y * s->pitch);
		for (int x = 0; x < s->w; ++x) {
			Uint32 p = src[x];
			Uint8 r = argb ? p >> 16 : p, g = p >> 8, b = argb ? p : p >> 16;
			line[x * 3] = b, line[x * 3 + 1] = g, line[x * 3 + 2] = r;
		}
		fwrite(line, 1, row, f);
	}
	free(line);
	return fclose(f) == 0 ? 0 : -1;
}

SDL_Surface* SDL_LoadBMP(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		set_error("no %s", path);
		return NULL;
	}
	Uint8 h[54];
	SDL_Surface* s = NULL;
	if (fread(h, 1, 54, f) == 54 && h[0] == 'B' && h[1] == 'M') {
		int off = h[10] | h[11] << 8 | h[12] << 16 | h[13] << 24;
		int w = h[18] | h[19] << 8 | h[20] << 16 | h[21] << 24;
		int ht = (int) (h[22] | h[23] << 8 | h[24] << 16 | (unsigned) h[25] << 24);
		int bpp = h[28] | h[29] << 8;
		bool down = ht < 0;
		if (down) ht = -ht;
		if (w > 0 && ht > 0 && w <= 4096 && ht <= 4096 && (bpp == 24 || bpp == 32)) {
			s = SDL_CreateRGBSurfaceWithFormat(0, w, ht, 32, SDL_PIXELFORMAT_ABGR8888);
			int row = (w * (bpp / 8) + 3) & ~3;
			Uint8* line = malloc(row);
			fseek(f, off, SEEK_SET);
			for (int y = 0; s && line && y < ht; ++y) {
				if (fread(line, 1, row, f) != (size_t) row) break;
				Uint32* dst = (Uint32*) s->pixels + (down ? y : ht - 1 - y) * w;
				for (int x = 0; x < w; ++x) {
					const Uint8* p = line + x * (bpp / 8);
					dst[x] = 0xFF000000u | (Uint32) p[0] << 16 | (Uint32) p[1] << 8 | p[2];
				}
			}
			free(line);
		}
	}
	fclose(f);
	if (!s) set_error("%s isn't a BMP this reads", path);
	return s;
}

/* ------------------------------------------------------------ video */

struct SDL_Window {
	int display;
	int w, h;
	SDL_Renderer* ren;
};

#define STAGE_SLOTS 3

struct SDL_Texture {
	C3D_Tex tex;
	C3D_RenderTarget* target; /* SDL_TEXTUREACCESS_TARGET */
	int w, h; /* as the app knows it */
	int tw, th; /* the texels used: smaller when w or h is past the GPU's 1024 */
	Uint32 format;
	SDL_BlendMode blend;
	SDL_ScaleMode scale;
	bool linear; /* the filter the GPU texture is set to */
	Uint8 r, g, b, a;
	bool streaming; /* updated again and again: tiled by the GPU */
	bool raw;       /* raw_picture: its words are the picture's own (bytes R, G, B, A), so it is drawn through a combiner swizzle */
	/* its picture in linear memory for the GPU's tiling: its own, and rows a frame's queued transfer is still to read are
	 * not written again in that frame (transfers queued in a frame run at its end) */
	u32* stage[STAGE_SLOTS];
	unsigned stageFrame[STAGE_SLOTS]; /* the frame the copies queued from a slot belong to ... */
	u64 stageBands[STAGE_SLOTS];      /* ... and the bands of 8 rows they read */
};

struct SDL_Renderer {
	SDL_Window* win;
	C3D_RenderTarget* screen;
	SDL_Texture* target; /* SDL_SetRenderTarget */
	int w, h;
	Uint8 r, g, b, a;
	SDL_BlendMode blend;
	bool clip;
	SDL_Rect clipRect;
	bool vsync;
	bool presented;
};

static bool videoUp;
static SDL_Window windows[2];
static SDL_Renderer renderers[2];
static bool inFrame;
static unsigned frameSerial; /* the frames begun: what a transfer queued in one is tagged with */
static C3D_RenderTarget* bound; /* the target of the scene under way */
static SDL_BlendMode boundBlend = SDL_BLENDMODE_BLEND;
static bool scissorOn;

int SDL_GetNumVideoDisplays(void) {
	return 2;
}

const char* SDL_GetCurrentVideoDriver(void) {
	return "n3ds";
}

int SDL_GetDisplayBounds(int display, SDL_Rect* r) {
	r->x = 0, r->y = display ? 240 : 0;
	r->w = display ? 320 : 400, r->h = 240;
	return 0;
}

const char* SDL_GetDisplayName(int display) {
	return display ? "bottom screen" : "top screen";
}

int SDL_GetDisplayDPI(int display, float* d, float* h, float* v) {
	(void) display;
	(void) d;
	(void) h;
	(void) v;
	return -1;
}

int SDL_GetDesktopDisplayMode(int display, SDL_DisplayMode* m) {
	memset(m, 0, sizeof(*m));
	m->w = display ? 320 : 400, m->h = 240;
	m->refresh_rate = 60;
	return 0;
}

int SDL_GetCurrentDisplayMode(int display, SDL_DisplayMode* m) {
	return SDL_GetDesktopDisplayMode(display, m);
}

int SDL_GetNumTouchDevices(void) {
	return 1;
}

void* SDL_GL_GetProcAddress(const char* name) {
	(void) name;
	return NULL;
}

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) {
	(void) title;
	(void) y;
	(void) w;
	(void) h;
	(void) flags;
	int display = ((Uint32) x & 0xFFFF0000u) == 0x2FFF0000u ? x & 0xFFFF : 0;
	if (display < 0 || display > 1) display = 0;
	SDL_Window* win = &windows[display];
	win->display = display;
	win->w = display ? 320 : 400;
	win->h = 240;
	return win;
}

void SDL_DestroyWindow(SDL_Window* w) {
	(void) w;
}

Uint32 SDL_GetWindowID(SDL_Window* w) {
	return w ? (Uint32) w->display + 1 : 0;
}

int SDL_GetWindowDisplayIndex(SDL_Window* w) {
	return w ? w->display : -1;
}

void SDL_GetWindowSize(SDL_Window* win, int* w, int* h) {
	if (w) *w = win->w;
	if (h) *h = win->h;
}

SDL_Renderer* SDL_CreateRenderer(SDL_Window* w, int index, Uint32 flags) {
	(void) index;
	SDL_Renderer* r = &renderers[w->display];
	memset(r, 0, sizeof(*r));
	r->win = w;
	w->ren = r;
	r->w = w->w, r->h = w->h;
	r->screen = C2D_CreateScreenTarget(w->display ? GFX_BOTTOM : GFX_TOP, GFX_LEFT);
	if (!r->screen) {
		set_error("no render target for the %s screen", w->display ? "bottom" : "top");
		return NULL;
	}
	r->a = 255;
	r->vsync = (flags & SDL_RENDERER_PRESENTVSYNC) != 0;
	return r;
}

SDL_Renderer* SDL_CreateSoftwareRenderer(SDL_Surface* s) {
	(void) s;
	set_error("no software renderer");
	return NULL;
}

void SDL_DestroyRenderer(SDL_Renderer* r) {
	(void) r;
}

int SDL_GetRendererInfo(SDL_Renderer* r, SDL_RendererInfo* info) {
	(void) r;
	memset(info, 0, sizeof(*info));
	info->name = "citro2d";
	info->max_texture_width = info->max_texture_height = 1024;
	return 0;
}

int SDL_GetRendererOutputSize(SDL_Renderer* r, int* w, int* h) {
	if (w) *w = r->target ? r->target->w : r->w;
	if (h) *h = r->target ? r->target->h : r->h;
	return 0;
}

int SDL_RenderSetVSync(SDL_Renderer* r, int vsync) {
	r->vsync = vsync != 0;
	return 0;
}

static void doomed_free(void);
static void doomed_targets_free(void);

/* The bench's "gpu wait": what the calls that can wait for the GPU take (C3D_FrameBegin and C3D_FrameEnd, a picture's
 * display transfer). */
static atomic_uint gpuWait;
static u64 waitBegin, waitEnd, waitCopy; /* (the same, by call: logged with the picture's upload) */
#define GPU_WAIT_IN(part, call) \
	do { \
		u64 waitT0 = svcGetSystemTick(); \
		call; \
		u64 waitDt = svcGetSystemTick() - waitT0; \
		atomic_fetch_add(&gpuWait, (unsigned) waitDt); \
		part += waitDt; \
	} while (0)
#define GPU_WAIT(call) GPU_WAIT_IN(waitCopy, call)

uint32_t host_gpu_wait_ticks(void) {
	return atomic_load(&gpuWait);
}

static void tex_verify(void);
static void rawcheck_verify(void);

static void frame_begin(void) {
	if (inFrame) return;
	bool sync = false;
	for (int i = 0; i < 2; ++i) {
		if (windows[i].ren && windows[i].ren->vsync) sync = true;
		if (windows[i].ren) windows[i].ren->presented = false;
	}
	doomed_targets_free(); /* (citro3d stops the app if a render target goes inside a frame) */
	GPU_WAIT_IN(waitBegin, C3D_FrameBegin(sync ? C3D_FRAME_SYNCDRAW : 0));
	tex_verify();
	rawcheck_verify();
	doomed_free();
	inFrame = true;
	++frameSerial;
	bound = NULL;
	boundBlend = SDL_BLENDMODE_BLEND;
	scissorOn = false;
}

static void set_blend(SDL_BlendMode mode) {
	if (mode == boundBlend) return;
	C2D_Flush();
	boundBlend = mode;
	switch (mode) {
	case SDL_BLENDMODE_NONE:
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		break;
	case SDL_BLENDMODE_MOD:
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_ZERO, GPU_ZERO, GPU_ONE);
		break;
	case SDL_BLENDMODE_ADD:
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
		break;
	default:
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
		break;
	}
}

/* The clip in the GPU's own terms: a screen's picture is stored on its
 * side (240 wide, the screen's width tall). */
static void apply_scissor(SDL_Renderer* r) {
	C2D_Flush();
	if (!r->clip) {
		C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
		scissorOn = false;
		return;
	}
	int tw = r->target ? r->target->w : r->w, th = r->target ? r->target->h : r->h;
	int x0 = SDL_clamp(r->clipRect.x, 0, tw), y0 = SDL_clamp(r->clipRect.y, 0, th);
	int x1 = SDL_clamp(r->clipRect.x + r->clipRect.w, x0, tw), y1 = SDL_clamp(r->clipRect.y + r->clipRect.h, y0, th);
	if (r->target) C3D_SetScissor(GPU_SCISSOR_NORMAL, x0, y0, x1, y1);
	else C3D_SetScissor(GPU_SCISSOR_NORMAL, th - y1, tw - x1, th - y0, tw - x0);
	scissorOn = true;
}

/* Makes r's target the one drawn to. */
static void scene(SDL_Renderer* r) {
	frame_begin();
	C3D_RenderTarget* want = r->target ? r->target->target : r->screen;
	if (bound != want) {
		C2D_SceneBegin(want);
		bound = want;
		if (r->clip || scissorOn) apply_scissor(r);
	}
}

int SDL_SetRenderDrawBlendMode(SDL_Renderer* r, SDL_BlendMode mode) {
	r->blend = mode;
	return 0;
}

int SDL_SetRenderDrawColor(SDL_Renderer* ren, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
	ren->r = r, ren->g = g, ren->b = b, ren->a = a;
	return 0;
}

int SDL_RenderClear(SDL_Renderer* r) {
	frame_begin();
	C3D_RenderTarget* t = r->target ? r->target->target : r->screen;
	if (bound == t) C2D_Flush();
	C2D_TargetClear(t, C2D_Color32(r->r, r->g, r->b, r->a));
	return 0;
}

void SDL_RenderPresent(SDL_Renderer* r) {
	if (!inFrame) return;
	r->presented = true;
	for (int i = 0; i < 2; ++i) {
		if (windows[i].ren && !windows[i].ren->presented) return;
	}
	C2D_Flush();
	if (boundBlend != SDL_BLENDMODE_BLEND) set_blend(SDL_BLENDMODE_BLEND);
	if (scissorOn) {
		C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
		scissorOn = false;
	}
	GPU_WAIT_IN(waitEnd, C3D_FrameEnd(0));
	inFrame = false;
	bound = NULL;
	static int frames;
	if (frames < 3) logLine("start: frame %d drawn", ++frames);
}

int SDL_RenderFlush(SDL_Renderer* r) {
	(void) r;
	if (inFrame) C2D_Flush();
	return 0;
}

int SDL_RenderSetClipRect(SDL_Renderer* r, const SDL_Rect* rect) {
	if (!rect && !r->clip) return 0;
	if (rect && r->clip && !memcmp(rect, &r->clipRect, sizeof(*rect))) return 0;
	r->clip = rect != NULL;
	if (rect) r->clipRect = *rect;
	C3D_RenderTarget* t = r->target ? r->target->target : r->screen;
	if (inFrame && bound == t) apply_scissor(r);
	return 0;
}

void SDL_RenderGetClipRect(SDL_Renderer* r, SDL_Rect* rect) {
	if (r->clip) *rect = r->clipRect;
	else memset(rect, 0, sizeof(*rect));
}

SDL_bool SDL_RenderIsClipEnabled(SDL_Renderer* r) {
	return r->clip ? SDL_TRUE : SDL_FALSE;
}

int SDL_RenderSetViewport(SDL_Renderer* r, const SDL_Rect* rect) {
	(void) r;
	(void) rect;
	return 0;
}

int SDL_RenderFillRects(SDL_Renderer* r, const SDL_Rect* rects, int n) {
	scene(r);
	set_blend(r->blend);
	u32 c = C2D_Color32(r->r, r->g, r->b, r->a);
	for (int i = 0; i < n; ++i) C2D_DrawRectSolid(rects[i].x, rects[i].y, 0.5f, rects[i].w, rects[i].h, c);
	return 0;
}

int SDL_RenderFillRect(SDL_Renderer* r, const SDL_Rect* rect) {
	SDL_Rect all = { 0, 0, 0, 0 };
	if (!rect) {
		SDL_GetRendererOutputSize(r, &all.w, &all.h);
		rect = &all;
	}
	return SDL_RenderFillRects(r, rect, 1);
}

/* The GPU reads a raw texture's words as RGBA8: red is the picture's alpha, green its blue, blue its green. Three combiner stages put them
 * right (each channel times an all-or-nothing mask, added up; a mix of two channels came out off by one on the console, this is exact there)
 * and citro2d's own first stage reads their result instead of the texture; the quad is drawn at once. */
static void raw_draw_flush(void) {
	C3D_TexEnv keep[4];
	C3D_TexEnv* env[4];
	for (int i = 0; i < 4; ++i) {
		env[i] = C3D_GetTexEnv(i);
		keep[i] = *env[i];
	}
	*env[3] = keep[0];
	for (int shift = 0; shift < 12; shift += 4) {
		if (((env[3]->srcRgb >> shift) & 0xF) == GPU_TEXTURE0) env[3]->srcRgb = (u16) ((env[3]->srcRgb & ~(0xF << shift)) | (GPU_PREVIOUS << shift));
		if (((env[3]->srcAlpha >> shift) & 0xF) == GPU_TEXTURE0) env[3]->srcAlpha = (u16) ((env[3]->srcAlpha & ~(0xF << shift)) | (GPU_PREVIOUS << shift));
	}
	static const GPU_TEVOP_RGB from[3] = { GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_B, GPU_TEVOP_RGB_SRC_G };
	for (int c = 0; c < 3; ++c) {
		C3D_TexEnvInit(env[c]);
		C3D_TexEnvSrc(env[c], C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
		C3D_TexEnvOpRgb(env[c], from[c], GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
		C3D_TexEnvFunc(env[c], C3D_RGB, c ? GPU_MULTIPLY_ADD : GPU_MODULATE);
		C3D_TexEnvColor(env[c], 0xFFu << (8 * c));
	}
	C3D_TexEnvSrc(env[0], C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR); /* (the alpha from the texel's red) */
	C3D_TexEnvOpAlpha(env[0], GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
	C2D_Flush();
	for (int i = 0; i < 4; ++i) C3D_SetTexEnv(i, &keep[i]);
}

int SDL_RenderCopy(SDL_Renderer* r, SDL_Texture* t, const SDL_Rect* src, const SDL_Rect* dst) {
	if (!t) return -1;
	scene(r);
	set_blend(t->blend);
	bool linear = t->scale != SDL_ScaleModeNearest;
	if (linear != t->linear) {
		C2D_Flush();
		C3D_TexSetFilter(&t->tex, linear ? GPU_LINEAR : GPU_NEAREST, linear ? GPU_LINEAR : GPU_NEAREST);
		t->linear = linear;
	}
	SDL_Rect whole = { 0, 0, t->w, t->h }, out = { 0, 0, 0, 0 };
	if (!src) src = &whole;
	if (!dst) {
		SDL_GetRendererOutputSize(r, &out.w, &out.h);
		dst = &out;
	}
	if (src->w <= 0 || src->h <= 0 || dst->w <= 0 || dst->h <= 0) return 0;
	/* the part of the texture, its top row at v = 1 */
	float kx = (float) t->tw / t->w / t->tex.width, ky = (float) t->th / t->h / t->tex.height;
	Tex3DS_SubTexture sub;
	sub.width = (u16) src->w;
	sub.height = (u16) src->h;
	sub.left = src->x * kx;
	sub.right = (src->x + src->w) * kx;
	sub.top = 1.0f - src->y * ky;
	sub.bottom = 1.0f - (src->y + src->h) * ky;
	/* (smooth scaling blends the last texel with the one past it: past the picture, a texture holds whatever VRAM had) */
	if (linear && src->x + src->w >= t->w && t->tw < t->tex.width) sub.right -= 0.5f / t->tex.width;
	if (linear && src->y + src->h >= t->h && t->th < t->tex.height) sub.bottom += 0.5f / t->tex.height;
	C2D_Image img = { &t->tex, &sub };
	C2D_ImageTint tint;
	bool tinted = t->a != 255 || t->r != 255 || t->g != 255 || t->b != 255;
	if (tinted) {
		/* (a colour mod stands for the texture's own colour: white shapes
		 * take it as SDL would give it) */
		bool coloured = t->r != 255 || t->g != 255 || t->b != 255;
		C2D_PlainImageTint(&tint, C2D_Color32(t->r, t->g, t->b, t->a), coloured ? 1.0f : 0.0f);
	}
	const bool queued = C2D_DrawImageAt(img, dst->x, dst->y, 0.5f, tinted ? &tint : NULL, (float) dst->w / src->w, (float) dst->h / src->h);
	if (t->raw && queued) raw_draw_flush();
	return 0;
}

int SDL_RenderReadPixels(SDL_Renderer* r, const SDL_Rect* rect, Uint32 format, void* pixels, int pitch) {
	(void) r;
	(void) rect;
	(void) format;
	(void) pixels;
	(void) pitch;
	set_error("the screens can't be read back");
	return -1;
}

SDL_bool SDL_RenderTargetSupported(SDL_Renderer* r) {
	(void) r;
	return SDL_TRUE;
}

int SDL_SetRenderTarget(SDL_Renderer* r, SDL_Texture* t) {
	if (t && !t->target) return -1;
	if (inFrame) C2D_Flush();
	r->target = t;
	return 0;
}

static int pow2(int v) {
	int p = 8;
	while (p < v) p <<= 1;
	return p;
}

/* Whole pictures on their way to the GPU's copy engine: two buffers, used in
 * turn. C3D_SyncDisplayTransfer can return with its copy still under way: it
 * waits for the GPU's PPF event, which the frame's own copies to the screens
 * have left set. With one buffer, the next picture (the game's, uploaded
 * right after the second screen's) was written over it while the copy engine
 * still read the last one, whose lower rows came out as the game's (on the
 * console; Azahar's copies finish at once). A buffer is filled again two
 * uploads later, after the upload between has waited for the GPU's queue
 * (C3D_SyncDisplayTransfer does that first), its copy with it. */
static u32* stages[2];
static size_t stageSizes[2];
static int stageNext;
static unsigned stagesFrame[2]; /* the frame a copy was last queued from each, in a frame */

/* Texel (x, y) of a w-wide texture in the GPU's layout: 8x8 tiles, row by
 * row, Morton order inside a tile; the first row is the picture's top. */
/* Memory the GPU or the DSP reads, written back from the CPU's cache: by the kernel itself (the GPU and
 * DSP services only make this same call, a round trip to another process away). */
static void cache_flush(const void* p, size_t n) {
	svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32) p, n);
}

static inline u32 tiled(u32 x, u32 y, u32 w) {
	return ((((y >> 3) * (w >> 3)) + (x >> 3)) << 6) |
	       ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
}

static int doomed_flush(void);

/* t's GPU texture (in VRAM for a render target, else linear memory). */
static bool texture_alloc(SDL_Texture* t, int access) {
	bool ok;
	if (access == SDL_TEXTUREACCESS_TARGET) {
		ok = C3D_TexInitVRAM(&t->tex, (u16) pow2(t->tw), (u16) pow2(t->th), GPU_RGBA8);
		if (ok) {
			t->target = C3D_RenderTargetCreateFromTex(&t->tex, GPU_TEXFACE_2D, 0, -1);
			if (!t->target) {
				C3D_TexDelete(&t->tex);
				ok = false;
			}
		}
	} else {
		ok = C3D_TexInit(&t->tex, (u16) pow2(t->tw), (u16) pow2(t->th), t->format == SDL_PIXELFORMAT_RGB565 ? GPU_RGB565 : GPU_RGBA8);
		if (ok) {
			memset(t->tex.data, 0, t->tex.size);
			cache_flush(t->tex.data, t->tex.size);
		}
	}
	return ok;
}

SDL_Texture* SDL_CreateTexture(SDL_Renderer* r, Uint32 format, int access, int w, int h) {
	(void) r;
	if (w <= 0 || h <= 0) {
		set_error("a %dx%d texture", w, h);
		return NULL;
	}
	SDL_Texture* t = calloc(1, sizeof(*t));
	if (!t) return NULL;
	t->w = w, t->h = h;
	t->tw = w, t->th = h;
	while (t->tw > 1024 || t->th > 1024) t->tw = (t->tw + 1) / 2, t->th = (t->th + 1) / 2;
	t->format = format;
	t->streaming = access == SDL_TEXTUREACCESS_STREAMING;
	t->blend = SDL_BLENDMODE_NONE;
	t->r = t->g = t->b = t->a = 255;
	bool ok = texture_alloc(t, access);
	if (!ok) {
		/* (a new game's textures are made while the last game's wait to be freed: its memory may be what's missing) */
		int freed = doomed_flush();
		if (freed) ok = texture_alloc(t, access);
		static int told; /* (a texture that keeps failing is asked for every frame) */
		if (told < 200) ++told, logLine("textures: no memory for a %dx%d texture, %d waiting ones freed: %s", w, h, freed, ok ? "made" : "not made");
	}
	if (!ok) {
		set_error("no memory for a %dx%d texture", w, h);
		free(t);
		return NULL;
	}
	C3D_TexSetFilter(&t->tex, GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	return t;
}

/* A texture the app lets go may be in the frame the GPU is still drawing:
 * it's freed once the next frame begins, which waits for that one. */
static SDL_Texture* doomed[256];
static int nDoomed;

static void tex_forget(const SDL_Texture* t);

static void texture_free(SDL_Texture* t) {
	tex_forget(t);
	for (int i = 0; i < STAGE_SLOTS; ++i) {
		if (t->stage[i]) linearFree(t->stage[i]);
	}
	if (t->target) C3D_RenderTargetDelete(t->target);
	C3D_TexDelete(&t->tex);
	free(t);
}

/* The render targets of the textures let go: between frames. */
static void doomed_targets_free(void) {
	for (int i = 0; i < nDoomed; ++i) {
		if (!doomed[i]->target) continue;
		C3D_RenderTargetDelete(doomed[i]->target);
		doomed[i]->target = NULL;
	}
}

static void doomed_free(void) {
	for (int i = 0; i < nDoomed; ++i) texture_free(doomed[i]);
	nDoomed = 0;
}

/* The textures let go, freed now: finish what's drawn, wait for the GPU, free. Returns how many. */
static int doomed_flush(void) {
	int n = nDoomed;
	if (!n) return 0;
	if (inFrame) {
		C2D_Flush();
		/* (as SDL_RenderPresent leaves them: frame_begin takes the blend and the scissor as these) */
		if (boundBlend != SDL_BLENDMODE_BLEND) set_blend(SDL_BLENDMODE_BLEND);
		if (scissorOn) {
			C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
			scissorOn = false;
		}
		C3D_FrameEnd(0);
		inFrame = false;
		bound = NULL;
	}
	C3D_FrameBegin(0);
	C3D_FrameEnd(0);
	doomed_free();
	return n;
}

void SDL_DestroyTexture(SDL_Texture* t) {
	if (!t) return;
	for (int i = 0; i < 2; ++i) {
		if (renderers[i].target == t) renderers[i].target = NULL;
	}
	if (t->target && bound == t->target) {
		C2D_Flush();
		bound = NULL;
	}
	/* (more than a frame ever lets go) */
	if (nDoomed == (int) SDL_arraysize(doomed)) doomed_flush();
	doomed[nDoomed++] = t;
}

int SDL_QueryTexture(SDL_Texture* t, Uint32* format, int* access, int* w, int* h) {
	if (!t) return -1;
	if (format) *format = t->format;
	if (access) *access = t->target ? SDL_TEXTUREACCESS_TARGET : SDL_TEXTUREACCESS_STREAMING;
	if (w) *w = t->w;
	if (h) *h = t->h;
	return 0;
}

static int update_by_cpu(SDL_Texture* t, const SDL_Rect* rect, const void* pixels, int pitch);

/* A whole picture is tiled by the GPU itself (GX_DisplayTransfer): the CPU only copies it into
 * linear memory in the GPU's byte order, where swizzling it pixel by pixel took several ms a frame.
 * (TEST: a file "cpu-tiling" in the app's folder turns it off, to compare.) */
static int gpuTiling = -1;

static bool gpu_tiling(void) {
	if (gpuTiling < 0) {
		FILE* f = fopen("/3ds/gba-ds/cpu-tiling", "r");
		gpuTiling = !f;
		if (f) fclose(f);
		logLine("textures: tiled by the %s", gpuTiling ? "GPU" : "CPU");
	}
	return gpuTiling;
}

/* gba-ds.cfg's async_upload: a picture's copy to its texture is queued in the frame being drawn and runs on the GPU while
 * the next frame emulates, where it was a call that waited for the copy engine (the same pixels, drawn at the same time). */
static bool asyncUpload;
static bool uploadSync; /* (the start's self-tests read the texture back at once) */

void host_set_async_upload(bool on) {
	asyncUpload = on;
}

static bool update_by_gpu(SDL_Texture* t, const void* pixels, int pitch, int y0, int rows);

/* (TEST: the GPU's tiling against the CPU's, word for word, once) */
static void tiling_selftest(void) {
	if (!gpu_tiling()) return;
	SDL_Texture* t = SDL_CreateTexture(NULL, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 240, 160);
	u32* px = malloc(240 * 160 * 4);
	if (!t || !px) return;
	for (int y = 0; y < 160; ++y) {
		for (int x = 0; x < 240; ++x) px[y * 240 + x] = (u32) x | (u32) y << 8 | 0x5A0000u | (u32) ((x * 7 + y) & 0xFF) << 24;
	}
	uploadSync = true;
	update_by_gpu(t, px, 240 * 4, 0, 160);
	uploadSync = false;
	svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32) t->tex.data, t->tex.size);
	const u32* got = t->tex.data;
	int bad = 0, first = -1;
	for (int y = 0; y < 160; ++y) {
		for (int x = 0; x < 240; ++x) {
			u32 want = __builtin_bswap32(px[y * 240 + x]);
			if (got[tiled((u32) x, (u32) y, t->tex.width)] != want) {
				if (first < 0) first = y * 240 + x;
				++bad;
			}
		}
	}
	if (first >= 0) {
		int fx = first % 240, fy = first / 240;
		logLine("textures: GPU tiling self-test: %d of 38400 texels differ; first (%d,%d) %08lx, wanted %08lx", bad, fx, fy,
		        (unsigned long) got[tiled((u32) fx, (u32) fy, t->tex.width)], (unsigned long) __builtin_bswap32(px[first]));
		/* where the first pixel went */
		u32 w0 = __builtin_bswap32(px[0]);
		for (u32 i = 0; i < t->tex.width * t->tex.height; ++i) {
			if (got[i] == w0) {
				logLine("textures: pixel (0,0) found at texel word %lu", (unsigned long) i);
				break;
			}
		}
	} else {
		logLine("textures: GPU tiling self-test: all 38400 texels match the CPU's layout");
	}
	free(px);
	texture_free(t);
}

/* gba-ds.cfg's cpu_tile: a whole 240x160 picture tiled by the CPU, straight into its texture, instead of through the
 * stage and the copy engine a call waits for. The frame is begun first: that waits for the GPU to finish the last
 * frame, which may still be drawing from this texture. */
static bool cpuTile, cpuTileOk;

void host_set_cpu_tile(bool on) {
	cpuTile = on;
}

/* texel (x, y) of a tile: the Morton order of 8x8 */
#define TILE_AT(x, y) (((x) & 1) | ((y) & 1) << 1 | ((x) & 2) << 1 | ((y) & 2) << 2 | ((x) & 4) << 2 | ((y) & 4) << 3)
#define TILE_ROW(y, f) \
	o[TILE_AT(0, y)] = f(s[(y) * sw + 0]), o[TILE_AT(1, y)] = f(s[(y) * sw + 1]), o[TILE_AT(2, y)] = f(s[(y) * sw + 2]), \
	o[TILE_AT(3, y)] = f(s[(y) * sw + 3]), o[TILE_AT(4, y)] = f(s[(y) * sw + 4]), o[TILE_AT(5, y)] = f(s[(y) * sw + 5]), \
	o[TILE_AT(6, y)] = f(s[(y) * sw + 6]), o[TILE_AT(7, y)] = f(s[(y) * sw + 7])

/* the GPU's RGBA8 has R in the top byte: ARGB8888 is rotated to it, ABGR8888 and XBGR8888 (alpha made full) swapped */
#define TO_RGBA_ARGB(p) ((p) << 8 | (p) >> 24)
#define TO_RGBA_ABGR(p) __builtin_bswap32(p)
#define TO_RGBA_XBGR(p) __builtin_bswap32((p) | 0xFF000000u)
#define TILE_ALL(f) \
	TILE_ROW(0, f), TILE_ROW(1, f), TILE_ROW(2, f), TILE_ROW(3, f), TILE_ROW(4, f), TILE_ROW(5, f), TILE_ROW(6, f), TILE_ROW(7, f)

static void tile_rgba8(u32* out, size_t outTilesX, const Uint32* src, size_t sw, int tilesX, int tilesY, Uint32 format) {
	for (int ty = 0; ty < tilesY; ++ty) {
		const Uint32* row = src + (size_t) ty * 8 * sw;
		u32* o = out + (size_t) ty * outTilesX * 64;
		for (int tx = 0; tx < tilesX; ++tx, o += 64) {
			const Uint32* s = row + tx * 8;
			if (format == SDL_PIXELFORMAT_ARGB8888) TILE_ALL(TO_RGBA_ARGB);
			else if (format == SDL_PIXELFORMAT_ABGR8888) TILE_ALL(TO_RGBA_ABGR);
			else TILE_ALL(TO_RGBA_XBGR);
		}
	}
}

static u64 upTileTicks;

/* (the texel columns past the picture, up to the texture's width, stay as they were: zero) */
static bool tile_into(SDL_Texture* t, const void* pixels, int pitch) {
	const bool known = t->format == SDL_PIXELFORMAT_XBGR8888 || t->format == SDL_PIXELFORMAT_ABGR8888 || t->format == SDL_PIXELFORMAT_ARGB8888;
	if (!known || !t->streaming || t->tw != t->w || t->th != t->h || (pitch & 3) || ((u32) pixels & 3) || (t->w & 7) || (t->h & 7) || t->tex.width < 64) {
		return false;
	}
	u64 t0 = svcGetSystemTick();
	int tilesX = t->w / 8, tilesY = t->h / 8;
	tile_rgba8(t->tex.data, t->tex.width / 8, pixels, (size_t) pitch / 4, tilesX, tilesY, t->format);
	u64 t1 = svcGetSystemTick();
	cache_flush(t->tex.data, (size_t) t->tex.width * 8 * 4 * (size_t) tilesY);
	upTileTicks += t1 - t0;
	return true;
}

static bool update_by_cpu_tiled(SDL_Texture* t, const void* pixels, int pitch) {
	if (!cpuTile || !cpuTileOk) return false;
	frame_begin();
	return tile_into(t, pixels, pitch);
}

/* A stage of t's whose rows (bands of 8, a bit each) no transfer queued in the frame under way reads. */
static u32* stage_take(SDL_Texture* t, size_t words, bool queued, u64 bands, int* slot) {
	for (int i = 0; i < STAGE_SLOTS; ++i) {
		const bool pending = queued && t->stage[i] && t->stageFrame[i] == frameSerial;
		if (pending && (t->stageBands[i] & bands)) continue;
		if (!t->stage[i]) {
			t->stage[i] = linearAlloc(words * 4);
			if (!t->stage[i]) continue;
			memset(t->stage[i], 0, words * 4);
		}
		t->stageBands[i] = (pending ? t->stageBands[i] : 0) | bands;
		*slot = i;
		return t->stage[i];
	}
	return NULL;
}

static u64 upSwapTicks, upFlushTicks8, upXferTicks8;

/* Rows y0 to y0 + rows - 1 (whole tile rows: multiples of 8) of the picture, from pixels, which starts at row y0. */
static bool update_by_gpu(SDL_Texture* t, const void* pixels, int pitch, int y0, int rows) {
	int w = t->w, h = t->h;
	u32 texW = t->tex.width;
	if (!gpu_tiling() || !t->streaming || t->tw != w || t->th != h || (w & 7) || (h & 7) || texW < 64) return false;
	/* (begun first: that waits for the GPU to be done with the last frame, whose transfers read the stage) */
	const bool async = asyncUpload && !uploadSync;
	if (async) frame_begin();
	int slot = 0;
	const int band0 = y0 / 8, band1 = (y0 + rows - 1) / 8;
	const u64 bands = band1 > 63 ? ~0ull : (2ull << band1) - (1ull << band0);
	u32* stage = stage_take(t, (size_t) texW * (size_t) h, inFrame, bands, &slot);
	if (!stage) return false;
	u64 t0 = svcGetSystemTick();
	bool argb = t->format == SDL_PIXELFORMAT_ARGB8888;
	u32 alpha = t->format == SDL_PIXELFORMAT_XBGR8888 ? 0xFF000000u : 0;
	for (int y = 0; y < rows; ++y) {
		const Uint32* src = (const Uint32*) ((const Uint8*) pixels + (size_t) y * pitch);
		u32* dst = stage + (size_t) (y0 + y) * texW;
		if (argb) {
			for (int x = 0; x < w; ++x) dst[x] = src[x] << 8 | src[x] >> 24;
		} else {
			for (int x = 0; x < w; ++x) dst[x] = __builtin_bswap32(src[x] | alpha);
		}
	}
	u64 t1 = svcGetSystemTick();
	cache_flush(stage + (size_t) y0 * texW, (size_t) rows * texW * 4);
	u64 t2 = svcGetSystemTick();
	if (inFrame) t->stageFrame[slot] = frameSerial;
	/* (a strip of tile rows is a stretch of the texture's memory of its own) */
	GPU_WAIT(C3D_SyncDisplayTransfer(stage + (size_t) y0 * texW, GX_BUFFER_DIM(texW, rows), (u32*) t->tex.data + (size_t) y0 * texW,
	                                 GX_BUFFER_DIM(texW, rows),
	                                 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	                                     GX_TRANSFER_OUT_TILED(1)));
	if (w == 240 && h == 160) upSwapTicks += t1 - t0, upFlushTicks8 += t2 - t1, upXferTicks8 += svcGetSystemTick() - t2;
	return true;
}

/* gba-ds.cfg's gpu_twin: the picture the render worker widened is also made by it in the GPU's byte order, in linear memory,
 * flushed by the worker (emu_armgba.c); the copy engine reads it from there, so the game thread does no swap and no flush.
 * host_picture_twin is told, each frame, which pixels (the XBGR ones) have that twin: a texture updated from them is copied from it. */
static bool twinOn, twinOk;
static const void* twinKey;
static const void* twinPx;
static unsigned twinFrame; /* the frame a twin's copy was queued in (one a frame: the next picture is written over it two frames later) */
static u64 upTwinTicks;

void host_set_gpu_twin(bool on) {
	twinOn = on;
}

bool host_gpu_twin_ready(void) {
	return twinOn && twinOk && gpu_tiling();
}

void host_picture_twin(const void* fb, const void* gpuOrder) {
	twinKey = fb;
	twinPx = gpuOrder;
}

void host_flush_data(const void* p, size_t n) {
	cache_flush(p, n);
}

uint64_t host_ticks(void) {
	return svcGetSystemTick();
}

void host_gpu_settle(void) {
	if (!videoUp || inFrame) return;
	C3D_FrameBegin(0);
	C3D_FrameEnd(0);
}

/* gba-ds.cfg's twin_check (a test): each twin is compared with the picture it is the twin of, word for word, before it is used;
 * one that differs is not used (the stage path draws the picture) and the count is logged. */
static bool twinCheck;
static unsigned twinChecked, twinDiffers;

void host_set_twin_check(bool on) {
	twinCheck = on;
}

static bool twin_matches(const u32* fb, const u32* twin) {
	for (int y = 0; y < 160; ++y) {
		for (int x = 0; x < 240; ++x) {
			if (twin[y * HOST_TWIN_STRIDE + x] != __builtin_bswap32(fb[y * 240 + x])) {
				if (twinDiffers++ < 3) {
					logLine("textures: twin check: frame %u differs at (%d,%d): picture %08lx, twin %08lx", twinChecked, x, y, (unsigned long) fb[y * 240 + x],
					        (unsigned long) twin[y * HOST_TWIN_STRIDE + x]);
				}
				return false;
			}
		}
	}
	return true;
}

/* gba-ds.cfg's tex_check (a test): what the GPU made of each 240x160 picture is read back from its texture once the next frame
 * begins (the queue is empty then) and compared word for word with the picture it was given: the view the CPU's checks can't have
 * of the copy engine (a stale twin, a stage written too soon, a missed cache line). */
static bool texCheck;
#define TEX_CHECKED 4
static struct {
	const SDL_Texture* t;
	u32* want; /* the texture's words in tile order, as the picture should have made them */
	bool have;
} texSeen[TEX_CHECKED];
static unsigned texChecked, texDiffers;

void host_set_tex_check(bool on) {
	texCheck = on;
}

static void tex_forget(const SDL_Texture* t) {
	for (int i = 0; i < TEX_CHECKED; ++i) {
		if (texSeen[i].t == t) texSeen[i].t = NULL, texSeen[i].have = false;
	}
}

static void tex_verify(void) {
	for (int i = 0; i < TEX_CHECKED; ++i) {
		if (!texSeen[i].have) continue;
		texSeen[i].have = false;
		const SDL_Texture* t = texSeen[i].t;
		svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32) t->tex.data, t->tex.size);
		const u32* got = t->tex.data;
		int bad = 0, firstX = 0, firstY = 0;
		for (int y = 0; y < 160; ++y) {
			for (int x = 0; x < 240; ++x) {
				if (got[tiled((u32) x, (u32) y, t->tex.width)] != texSeen[i].want[y * 240 + x] && !bad++) firstX = x, firstY = y;
			}
		}
		++texChecked;
		if (bad && texDiffers++ < 20) logLine("textures: tex check: picture %u differs in %d words, the first at (%d,%d)", texChecked, bad, firstX, firstY);
		if (texChecked % 600 == 0) logLine("textures: tex check: %u pictures read back, %u differed", texChecked, texDiffers);
	}
}

/* (pixels as they were given: the words the texture should hold are the same swap as the stage's) */
static void tex_note(const SDL_Texture* t, const void* pixels, int pitch) {
	if (!texCheck || !pixels || (pitch != 240 * 4 && !(t->raw && pitch == HOST_TWIN_STRIDE * 4)) || t->w != 240 || t->h != 160 || !t->streaming || t->tex.width < 240) return;
	if (t->format != SDL_PIXELFORMAT_ABGR8888 && t->format != SDL_PIXELFORMAT_XBGR8888) return;
	int k = -1;
	for (int i = 0; i < TEX_CHECKED && k < 0; ++i) k = texSeen[i].t == t ? i : -1;
	for (int i = 0; i < TEX_CHECKED && k < 0; ++i) k = texSeen[i].t ? -1 : i;
	if (k < 0) return;
	if (!texSeen[k].want && !(texSeen[k].want = malloc(240 * 160 * sizeof(u32)))) return;
	const u32 alpha = t->format == SDL_PIXELFORMAT_XBGR8888 ? 0xFF000000u : 0;
	const u32* src = pixels;
	const int pw = pitch / 4;
	for (int i = 0; i < 240 * 160; ++i) {
		const u32 w = src[i / 240 * pw + i % 240];
		texSeen[k].want[i] = t->raw ? w : __builtin_bswap32(w | alpha);
	}
	texSeen[k].t = t;
	texSeen[k].have = true;
}

static bool update_from_twin(SDL_Texture* t, const void* pixels) {
	if (!pixels || pixels != twinKey || !twinPx || !twinOn || !twinOk) return false;
	if (twinCheck) {
		++twinChecked;
		const bool same = twin_matches(pixels, twinPx);
		if (twinChecked % 600 == 0) logLine("textures: twin check: %u pictures compared, %u differed", twinChecked, twinDiffers);
		if (!same) return false;
	}
	u32 texW = t->tex.width;
	if (t->format != SDL_PIXELFORMAT_ABGR8888 || !t->streaming || t->w != 240 || t->h != 160 || t->tw != 240 || t->th != 160 ||
	    texW != HOST_TWIN_STRIDE || !gpu_tiling()) {
		return false;
	}
	if (asyncUpload && !uploadSync) frame_begin();
	if (inFrame) {
		if (twinFrame == frameSerial) return false;
		twinFrame = frameSerial;
	}
	u64 t0 = svcGetSystemTick();
	GPU_WAIT(C3D_SyncDisplayTransfer((u32*) twinPx, GX_BUFFER_DIM(texW, 160), (u32*) t->tex.data, GX_BUFFER_DIM(texW, 160),
	                                 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	                                     GX_TRANSFER_OUT_TILED(1)));
	upTwinTicks += svcGetSystemTick() - t0;
	return true;
}

/* (TEST, at the start: another core writes a picture over old words and flushes it, as the render worker does; the
 * copy engine's tiling of it must be the new words. Off for the run if it is not.) */
typedef struct {
	u32* buf;
	u32 seed;
} TwinWriter;

static void twin_writer(void* arg) {
	TwinWriter* w = arg;
	u32 seed = w->seed;
	for (int y = 0; y < 160; ++y) {
		u32* row = w->buf + (size_t) y * HOST_TWIN_STRIDE;
		for (int x = 0; x < 240; ++x) row[x] = seed = seed * 1664525u + 1013904223u;
	}
	cache_flush(w->buf, (size_t) 160 * HOST_TWIN_STRIDE * 4);
}

static void twin_selftest(void) {
	twinOk = false;
	if (!gpu_tiling()) return;
	u32* buf = linearAlloc((size_t) HOST_TWIN_STRIDE * 160 * 4);
	SDL_Texture* t = SDL_CreateTexture(NULL, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 240, 160);
	if (!buf || !t || t->tex.width != HOST_TWIN_STRIDE) {
		if (buf) linearFree(buf);
		if (t) texture_free(t);
		logLine("textures: GPU twin self-test: not made");
		return;
	}
	memset(buf, 0, (size_t) HOST_TWIN_STRIDE * 160 * 4);
	cache_flush(buf, (size_t) HOST_TWIN_STRIDE * 160 * 4);
	int bad = 0, made = 0;
	for (int k = 0; k < 4; ++k) {
		TwinWriter w = { buf, 4242u + (u32) k * 77u };
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		Thread th = threadCreate(twin_writer, &w, 32 * 1024, prio, host_side_core(), false);
		if (!th) continue;
		threadJoin(th, U64_MAX);
		threadFree(th);
		++made;
		C3D_SyncDisplayTransfer(buf, GX_BUFFER_DIM(HOST_TWIN_STRIDE, 160), (u32*) t->tex.data, GX_BUFFER_DIM(HOST_TWIN_STRIDE, 160),
		                        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
		                            GX_TRANSFER_OUT_TILED(1));
		svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32) t->tex.data, t->tex.size);
		const u32* got = t->tex.data;
		for (int y = 0; y < 160; ++y) {
			for (int x = 0; x < HOST_TWIN_STRIDE; ++x) bad += got[tiled((u32) x, (u32) y, HOST_TWIN_STRIDE)] != buf[(size_t) y * HOST_TWIN_STRIDE + x];
		}
	}
	linearFree(buf);
	texture_free(t);
	twinOk = made == 4 && !bad;
	logLine("textures: GPU twin self-test: %s", made < 4 ? "no thread" : bad ? "differs from what the other core wrote" : "all words match (4 pictures, written on another core)");
}

/* gba-ds.cfg's raw_picture: the render worker's picture (linear memory it writes back itself) is copied to the texture as it is, and the GPU
 * puts the channels right when it draws (raw_draw_flush). host_picture_raw is told, each frame, which pixels are such a picture. */
static bool rawOn, rawOk, rawCheckOn;
static int rawMode; /* 1: the picture's rows 256 words apart, one transfer tiles it; 2: rows 240 apart, a copy that leaves gaps, then the transfer */
static signed char rawVerdict[3]; /* the self-test of each mode: 1 passed, -1 failed, 0 not run */
static const void* rawKey;
static u64 upRawTicks;

void host_set_raw_picture(int mode) {
	rawMode = mode >= 2 ? 2 : mode ? 1 : 0;
	rawOn = rawMode != 0;
	rawOk = rawVerdict[rawMode] > 0;
}

/* The words a row of the picture buffers has in this mode. */
int host_raw_picture_pitch(void) {
	return rawMode == 2 ? 240 : HOST_TWIN_STRIDE;
}

void host_set_raw_check(bool on) {
	rawCheckOn = on;
}

static void raw_selftest(void);
static void frame_end_now(void);

/* The self-test runs once for each mode, the first time a game asks with the option on: the copy's two dimensions are not what the GPU's
 * documentation covers, so a build or job that doesn't use raw_picture never sends it. */
bool host_raw_picture_ready(void) {
	if (!rawOn || !gpu_tiling()) return false;
	if (!rawVerdict[rawMode]) {
		frame_end_now(); /* (the first job of a run finds the start's last frame still open: the test begins frames of its own) */
		raw_selftest();
	}
	return rawOk;
}

void host_picture_raw(const void* fb) {
	rawKey = fb;
}

/* One transfer tiles a picture whose rows are 256 words apart, as the texture's are. With rows of 240 (raw_picture = 2) it takes two passes: a linear
 * copy that leaves a gap after each row, then the usual transfer. (One transfer from rows of 240 into the 256 wide texture is not something the
 * console's copy engine does: the rows came out wrong.) */
#define RAW_ROW_UNITS (240 * 4 / 16) /* (the copy engine counts a row and its gap in 16-byte units) */
#define RAW_GAP_UNITS ((HOST_TWIN_STRIDE - 240) * 4 / 16)
#define RAW_TILE_ROWS ((1ull << (160 / 8)) - 1)
static bool raw_transfer(SDL_Texture* t, const void* pixels) {
	const u32 flags = GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_TILED(1);
	if (rawMode == 2) {
		int slot = 0;
		u32* stage = stage_take(t, (size_t) HOST_TWIN_STRIDE * 160, inFrame, RAW_TILE_ROWS, &slot);
		if (!stage) return false;
		if (inFrame) t->stageFrame[slot] = frameSerial;
		GPU_WAIT(C3D_SyncTextureCopy((u32*) pixels, GX_BUFFER_DIM(RAW_ROW_UNITS, 0), stage, GX_BUFFER_DIM(RAW_ROW_UNITS, RAW_GAP_UNITS),
		                             240 * 160 * 4, 8 | 4));
		pixels = stage;
	}
	GPU_WAIT(C3D_SyncDisplayTransfer((u32*) pixels, GX_BUFFER_DIM(HOST_TWIN_STRIDE, 160), (u32*) t->tex.data, GX_BUFFER_DIM(HOST_TWIN_STRIDE, 160), flags));
	t->raw = true;
	return true;
}

/* A picture drawn both ways into targets of their own, to be compared once the GPU is done: swapped (the old way) and raw, each 1:1 and
 * 400x240 smooth. */
typedef struct {
	SDL_Texture* swapped;
	SDL_Texture* target[2][2]; /* [draw][0 swapped, 1 raw] */
	u32* read[2][2];           /* each target's words copied to linear memory by the copy engine, for the CPU to compare */
} RawRig;
static SDL_Renderer rawFake;
static const SDL_Rect rawRigDst[2] = { { 0, 0, 240, 160 }, { 0, 0, 400, 240 } };

static void rig_free(RawRig* g) {
	if (g->swapped) texture_free(g->swapped);
	for (int c = 0; c < 2; ++c) {
		for (int k = 0; k < 2; ++k) {
			if (g->target[c][k]) texture_free(g->target[c][k]);
			if (g->read[c][k]) linearFree(g->read[c][k]);
		}
	}
	memset(g, 0, sizeof(*g));
}

static bool rig_make(RawRig* g) {
	memset(g, 0, sizeof(*g));
	g->swapped = SDL_CreateTexture(NULL, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 240, 160);
	bool ok = g->swapped != NULL;
	for (int c = 0; c < 2; ++c) {
		for (int k = 0; k < 2; ++k) {
			g->target[c][k] = SDL_CreateTexture(NULL, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, rawRigDst[c].w, rawRigDst[c].h);
			ok = ok && g->target[c][k] != NULL;
			if (g->target[c][k]) ok = ok && (g->read[c][k] = linearMemAlign(g->target[c][k]->tex.size, 0x80)) != NULL;
		}
	}
	if (!ok) rig_free(g);
	return ok;
}

/* The targets' words to linear memory: the CPU reads VRAM through its cache, which may hold words from before the GPU drew, so the copy
 * engine reads them (queued in the frame under way; they are there once the GPU is done with it). */
static void rig_fetch(RawRig* g) {
	for (int c = 0; c < 2; ++c) {
		for (int k = 0; k < 2; ++k) {
			C3D_SyncTextureCopy((u32*) g->target[c][k]->tex.data, 0, g->read[c][k], 0, g->target[c][k]->tex.size, GX_TRANSFER_RAW_COPY(1));
		}
	}
}

/* In a frame, after t (a raw texture) has been updated from `picture` (its words in the bytes R, G, B, A, `pitch` bytes to a row). */
static void rig_draw(RawRig* g, SDL_Texture* t, const void* picture, int pitch) {
	update_by_gpu(g->swapped, picture, pitch, 0, 160);
	g->swapped->raw = false;
	C2D_Flush();
	if (scissorOn) C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0); /* (scene() puts the game's clip back) */
	const SDL_ScaleMode scale = t->scale;
	SDL_Renderer* r = &rawFake;
	SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
	for (int c = 0; c < 2; ++c) {
		for (int k = 0; k < 2; ++k) {
			SDL_Texture* from = k ? t : g->swapped;
			from->scale = c ? SDL_ScaleModeLinear : SDL_ScaleModeNearest;
			SDL_SetRenderTarget(r, g->target[c][k]);
			SDL_RenderClear(r);
			SDL_RenderCopy(r, from, NULL, &rawRigDst[c]);
		}
	}
	SDL_SetRenderTarget(r, NULL);
	g->swapped->scale = t->scale = scale;
	bound = NULL;
}

/* The words that differ between each draw's two targets, in the tile rows the draw covers (per draw, and the first of each: the word's place
 * in the target, the swapped draw's and the raw one's); the total. */
typedef struct {
	int bad[2];
	u32 at[2], swapped[2], raw[2];
} RawDiff;

static int rig_compare(RawRig* g, RawDiff* d) {
	RawDiff local;
	if (!d) d = &local;
	memset(d, 0, sizeof(*d));
	for (int c = 0; c < 2; ++c) {
		SDL_Texture* a = g->target[c][0];
		const size_t words = (size_t) (a->tex.width / 8) * 64 * (size_t) ((rawRigDst[c].h + 7) / 8);
		svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32) g->read[c][0], a->tex.size);
		svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32) g->read[c][1], a->tex.size);
		const u32 *ga = g->read[c][0], *gb = g->read[c][1];
		for (size_t i = 0; i < words; ++i) {
			if (ga[i] == gb[i]) continue;
			if (!d->bad[c]++) d->at[c] = (u32) i, d->swapped[c] = ga[i], d->raw[c] = gb[i];
		}
	}
	return d->bad[0] + d->bad[1];
}

/* raw_check: one picture a frame is drawn both ways and compared when the next frame begins. */
static RawRig checkRig;
static bool checkRigMade;
static int checkState; /* 0 nothing to compare; 1 drawn; 2 copied to linear memory (compared when the GPU is done with that frame) */
static unsigned rawChecked, rawDiffers;

static void raw_check_picture(SDL_Texture* t, const void* pixels, int pitch) {
	if (checkState) return;
	if (!checkRigMade && !(checkRigMade = rig_make(&checkRig))) {
		rawCheckOn = false;
		logLine("textures: raw check: no memory for the targets, off");
		return;
	}
	rig_draw(&checkRig, t, pixels, pitch);
	checkState = 1;
}

static bool gpu_settle_within(unsigned ms);

/* The test keys' verdicts so far (the last pictures' too), then counted again; the keys and the targets are let go, so the next job's lines
 * are its own. */
void host_checks_report(void) {
	char line[160];
	if (texCheck || rawCheckOn) frame_end_now(); /* (the last pictures' copies run when their frame ends) */
	host_gpu_settle();
	tex_verify();
	if (checkState == 1) {
		frame_begin();
		rig_fetch(&checkRig);
		frame_end_now();
		checkState = 2;
	}
	if (checkState == 2) {
		gpu_settle_within(3000);
		rawcheck_verify();
	}
	if (texChecked) {
		snprintf(line, sizeof(line), "tex check: %u pictures read back, %u differed", texChecked, texDiffers);
		logLine("textures: %s", line);
		if (bench_active()) bench_note(line);
	}
	if (rawChecked) {
		snprintf(line, sizeof(line), "raw check: %u pictures drawn both ways, %u differed", rawChecked, rawDiffers);
		logLine("textures: %s", line);
		if (bench_active()) bench_note(line);
	}
	texChecked = texDiffers = rawChecked = rawDiffers = 0;
	texCheck = rawCheckOn = false;
	if (checkRigMade && !inFrame) {
		rig_free(&checkRig);
		checkRigMade = false;
	}
}

static void raw_said(const char* line);

/* A picture drawn differently is told in detail (the first few): the draws' word counts, the first word of each, and the same read again (a
 * difference that is gone on the second read was the CPU's view of the targets, not the GPU's drawing). */
static void rawcheck_verify(void) {
	if (!checkState) return;
	if (checkState == 1) { /* (the GPU is done with the draws: the targets are copied now, and compared a frame on) */
		rig_fetch(&checkRig);
		checkState = 2;
		return;
	}
	checkState = 0;
	RawDiff d;
	++rawChecked;
	if (!rig_compare(&checkRig, &d)) {
		if (rawChecked % 600 == 0) logLine("textures: raw check: %u pictures drawn both ways, %u differed", rawChecked, rawDiffers);
		return;
	}
	if (rawDiffers++ < 8) {
		RawDiff again;
		rig_compare(&checkRig, &again);
		char line[300];
		snprintf(line, sizeof(line), "raw check: picture %u (frame %u) differs: %d words in the 1:1 draw, %d in the smooth one; first 1:1 word %lu swapped %08lx raw %08lx, first smooth word %lu swapped %08lx raw %08lx; read again: %d and %d",
		         rawChecked, frameSerial, d.bad[0], d.bad[1], (unsigned long) d.at[0], (unsigned long) d.swapped[0], (unsigned long) d.raw[0], (unsigned long) d.at[1],
		         (unsigned long) d.swapped[1], (unsigned long) d.raw[1], again.bad[0], again.bad[1]);
		raw_said(line);
	}
}

static bool update_from_raw(SDL_Texture* t, const void* pixels, int pitch) {
	if (!pixels || pixels != rawKey || !rawOn || !rawOk || pitch != host_raw_picture_pitch() * 4 || t->blend != SDL_BLENDMODE_NONE) return false;
	if (t->format != SDL_PIXELFORMAT_ABGR8888 || !t->streaming || t->w != 240 || t->h != 160 || t->tw != 240 || t->th != 160 ||
	    t->tex.width != HOST_TWIN_STRIDE || !gpu_tiling()) {
		return false;
	}
	if (asyncUpload && !uploadSync) frame_begin();
	if (inFrame) C2D_Flush(); /* (a draw of the texture still waiting would use the old stages on the raw words) */
	u64 t0 = svcGetSystemTick();
	if (!raw_transfer(t, pixels)) return false;
	upRawTicks += svcGetSystemTick() - t0;
	if (rawCheckOn && inFrame) raw_check_picture(t, pixels, pitch);
	return true;
}

/* (TEST: another core writes a picture and writes it back, as the render worker does; it is copied raw and drawn next to the same picture
 * swapped, and every word must be the same. Off if not.) */
typedef struct {
	u32* buf;
	u32* copy;
	int pitch;
	u32 seed;
} RawWriter;

static void raw_writer(void* arg) {
	RawWriter* w = arg;
	u32 seed = w->seed;
	for (int i = 0; i < 240 * 160; ++i) {
		const int x = i % 240, y = i / 240;
		/* (the first 256 hold every byte value in each channel, each in another order) */
		const u32 word = i < 256 ? (u32) i | (u32) ((i + 37) & 255) << 8 | (u32) ((i * 5 + 11) & 255) << 16 | (u32) (255 - i) << 24 : (seed = seed * 1664525u + 1013904223u);
		w->buf[y * w->pitch + x] = word;
		w->copy[i] = word;
	}
	cache_flush(w->buf, (size_t) (w->pitch * 160 + HOST_RAW_SLACK_WORDS) * 4);
}

static void frame_end_now(void) {
	if (!inFrame) return;
	C2D_Flush();
	if (boundBlend != SDL_BLENDMODE_BLEND) set_blend(SDL_BLENDMODE_BLEND);
	if (scissorOn) {
		C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
		scissorOn = false;
	}
	C3D_FrameEnd(0);
	inFrame = false;
	bound = NULL;
}

/* host_gpu_settle with a limit: a copy the GPU can't finish must not hang the app before it says so. */
static bool gpu_settle_within(unsigned ms) {
	for (unsigned i = 0; i < ms; ++i) {
		if (C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
			C3D_FrameEnd(0);
			return true;
		}
		svcSleepThread(1000000);
	}
	return false;
}

/* The self-test's verdict, in the log and (a long job's log can be cut) in the bench's results, under the job that ran it. */
static void raw_said(const char* line) {
	logLine("textures: %s", line);
	if (bench_active()) bench_note(line);
}

static void raw_selftest(void) {
	const int mode = rawMode;
	const int pitch = host_raw_picture_pitch();
	const char* how = mode == 2 ? "two passes" : "rows 256 apart";
	char line[300];
	rawOk = false;
	if (!gpu_tiling()) return;
	const size_t bytes = (size_t) (pitch * 160 + HOST_RAW_SLACK_WORDS) * 4;
	u32* buf = linearMemAlign(bytes, 0x80);
	u32* copy = malloc(240 * 160 * 4);
	SDL_Texture* t = SDL_CreateTexture(NULL, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 240, 160);
	RawRig g;
	if (!buf || !copy || !t || t->tex.width != HOST_TWIN_STRIDE || !rig_make(&g)) {
		if (buf) linearFree(buf);
		free(copy);
		if (t) texture_free(t);
		rawVerdict[mode] = -1;
		raw_said("raw picture self-test: not made");
		return;
	}
	memset(buf, 0, bytes);
	cache_flush(buf, bytes);
	const bool wasOn = rawOn, wasCheck = rawCheckOn;
	const void* wasKey = rawKey;
	rawOn = rawOk = true;
	rawCheckOn = false; /* (the test's own pictures are not the game's: raw_check would count a failing setup's as the game's) */
	int made = 0, badCopy = 0, badDraw = 0, whichDraw = 0;
	for (int k = 0; k < 2; ++k) {
		RawWriter w = { buf, copy, pitch, 9001u + (u32) k * 313u };
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		Thread th = threadCreate(raw_writer, &w, 32 * 1024, prio, host_side_core(), false);
		if (!th) continue;
		threadJoin(th, U64_MAX);
		threadFree(th);
		++made;
		frame_begin();
		rawKey = buf;
		const int rc = SDL_UpdateTexture(t, NULL, buf, pitch * 4);
		rawKey = NULL;
		if (rc || !t->raw) {
			frame_end_now();
			++badCopy;
			break;
		}
		rig_draw(&g, t, buf, pitch * 4);
		frame_end_now();
		if (!gpu_settle_within(3000)) {
			snprintf(line, sizeof(line), "raw picture self-test (%s): the GPU did not finish the pictures in 3 s, the copy or the drawing hangs it", how);
			raw_said(line);
			rawOn = wasOn, rawCheckOn = wasCheck;
			rawOk = false;
			rawVerdict[mode] = -1;
			rawKey = wasKey;
			return; /* (the targets are not freed: the GPU may still hold them) */
		}
		frame_begin();
		rig_fetch(&g);
		frame_end_now();
		if (!gpu_settle_within(3000)) {
			snprintf(line, sizeof(line), "raw picture self-test (%s): the GPU did not finish copying the drawings in 3 s", how);
			raw_said(line);
			rawOn = wasOn, rawCheckOn = wasCheck;
			rawOk = false;
			rawVerdict[mode] = -1;
			rawKey = wasKey;
			return;
		}
		svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32) t->tex.data, t->tex.size);
		const u32* got = t->tex.data;
		for (int y = 0; y < 160; ++y) {
			for (int x = 0; x < 240; ++x) badCopy += got[tiled((u32) x, (u32) y, t->tex.width)] != copy[y * 240 + x];
		}
		RawDiff d;
		const int bad = rig_compare(&g, &d);
		if (bad && !badDraw) whichDraw = d.bad[0] ? 0 : 1;
		badDraw += bad;
	}
	rawOn = wasOn, rawCheckOn = wasCheck;
	rawKey = wasKey;
	rig_free(&g);
	texture_free(t);
	linearFree(buf);
	free(copy);
	rawOk = made == 2 && !badCopy && !badDraw;
	rawVerdict[mode] = rawOk ? 1 : -1;
	if (rawOk) snprintf(line, sizeof(line), "raw picture self-test (%s): all words match (2 pictures, written on another core: copied as they are, drawn 1:1 and smooth, the same as swapped)", how);
	else if (made < 2) snprintf(line, sizeof(line), "raw picture self-test (%s): no thread", how);
	else snprintf(line, sizeof(line), "raw picture self-test (%s): %s (%d words of the copy, %d of the drawing, the first in the %s draw)", how,
	              badCopy ? "the copy differs" : "the drawing differs", badCopy, badDraw, whichDraw ? "smooth" : "1:1");
	raw_said(line);
}

/* (TEST: the average time of a whole picture's upload, logged now and then) */
static u64 upTicks, upFlushTicks, upXferTicks;
static unsigned upCount;

/* A whole RGB565 picture: the GPU reads it where it is (linear memory, a texture-wide row), else from a copy in the stage. */
static int update_565(SDL_Texture* t, const void* pixels, int pitch) {
	int w = t->w, h = t->h;
	u32 texW = t->tex.width;
	if (!gpu_tiling() || !t->streaming || t->tw != w || t->th != h || (w & 7) || (h & 7) || texW < 64) {
		u16* out = t->tex.data;
		for (int y = 0; y < h && y < t->th; ++y) {
			const u16* src = (const u16*) ((const u8*) pixels + (size_t) y * pitch);
			for (int x = 0; x < w && x < t->tw; ++x) out[tiled((u32) x, (u32) y, texW)] = src[x];
		}
		cache_flush(t->tex.data, t->tex.size);
		return 0;
	}
	const void* from = pixels;
	if (pitch != (int) (texW * 2) || !osConvertVirtToPhys(pixels) || ((u32) pixels & 0x7F)) {
		if (!t->stage[0]) {
			t->stage[0] = linearAlloc((size_t) texW * h * 2);
			if (!t->stage[0]) return -1;
			memset(t->stage[0], 0, (size_t) texW * h * 2);
		}
		for (int y = 0; y < h; ++y) memcpy((u16*) t->stage[0] + (size_t) y * texW, (const u8*) pixels + (size_t) y * pitch, (size_t) w * 2);
		from = t->stage[0];
	}
	u64 flushT0 = svcGetSystemTick();
	cache_flush(from, (size_t) texW * h * 2);
	u64 xferT0 = svcGetSystemTick();
	GPU_WAIT(C3D_SyncDisplayTransfer((u32*) from, GX_BUFFER_DIM(texW, h), (u32*) t->tex.data, GX_BUFFER_DIM(texW, h),
	                                 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
	                                     GX_TRANSFER_OUT_TILED(1)));
	if (t->w == 240 && t->h == 160) {
		upFlushTicks += xferT0 - flushT0;
		upXferTicks += svcGetSystemTick() - xferT0;
	}
	return 0;
}

/* (TEST: the CPU's tiling of a picture against the GPU's, word for word, for each format; the CPU's is used only if they match) */
static void cpu_tile_selftest(void) {
	if (!gpu_tiling()) return;
	static const Uint32 formats[3] = { SDL_PIXELFORMAT_XBGR8888, SDL_PIXELFORMAT_ABGR8888, SDL_PIXELFORMAT_ARGB8888 };
	u32* px = malloc(240 * 160 * 4);
	bool ok = px != NULL;
	int bad = 0;
	for (int k = 0; k < 3 && ok; ++k) {
		SDL_Texture* a = SDL_CreateTexture(NULL, formats[k], SDL_TEXTUREACCESS_STREAMING, 240, 160);
		SDL_Texture* b = SDL_CreateTexture(NULL, formats[k], SDL_TEXTUREACCESS_STREAMING, 240, 160);
		u32 seed = 777 + (u32) k;
		for (int i = 0; i < 240 * 160; ++i) px[i] = seed = seed * 1664525u + 1013904223u;
		uploadSync = true;
		bool viaGpu = a && b && update_by_gpu(a, px, 240 * 4, 0, 160);
		uploadSync = false;
		if (!viaGpu || !tile_into(b, px, 240 * 4)) {
			ok = false;
		} else {
			svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32) a->tex.data, a->tex.size);
			const u32 *ga = a->tex.data, *gb = b->tex.data;
			for (u32 i = 0; i < a->tex.width * 160; ++i) bad += ga[i] != gb[i];
		}
		if (a) texture_free(a);
		if (b) texture_free(b);
	}
	free(px);
	cpuTileOk = ok && !bad;
	logLine("textures: CPU tiling self-test: %s", !ok ? "not made" : bad ? "differs from the GPU's" : "all words match the GPU's (three formats)");
	upTileTicks = 0;
}

int SDL_UpdateTexture(SDL_Texture* t, const SDL_Rect* rect, const void* pixels, int pitch) {
	if (!t || t->target || !pixels) return -1;
	t->raw = false;
	SDL_Rect whole = { 0, 0, t->w, t->h };
	if (!rect) rect = &whole;
	if (t->format == SDL_PIXELFORMAT_RGB565) {
		if (rect != &whole && (rect->x || rect->y || rect->w != t->w || rect->h != t->h)) return -1;
		u64 t0 = svcGetSystemTick();
		int r = update_565(t, pixels, pitch);
		if (t->w == 240 && t->h == 160) {
			upTicks += svcGetSystemTick() - t0;
			if (++upCount == 600) {
				logLine("textures: a 240x160 picture's upload %.3f ms on average (cache flush %.3f, GPU transfer %.3f)",
				        (double) upTicks / upCount / CPU_TICKS_PER_MSEC, (double) upFlushTicks / upCount / CPU_TICKS_PER_MSEC,
				        (double) upXferTicks / upCount / CPU_TICKS_PER_MSEC);
				upTicks = upFlushTicks = upXferTicks = 0, upCount = 0;
			}
		}
		return r;
	}
	if (rect == &whole || (!rect->x && !rect->y && rect->w == t->w && rect->h == t->h)) {
		u64 t0 = svcGetSystemTick();
		bool done = t->w == 240 && t->h == 160 && update_by_cpu_tiled(t, pixels, pitch);
		if (!done && t->w == 240 && t->h == 160) done = update_from_twin(t, pixels);
		if (!done && t->w == 240 && t->h == 160) done = update_from_raw(t, pixels, pitch);
		if (!done) done = update_by_gpu(t, pixels, pitch, 0, t->h);
		if (!done) done = update_by_cpu(t, rect, pixels, pitch) == 0;
		if (done) tex_note(t, pixels, pitch);
		if (t->w == 240 && t->h == 160) {
			upTicks += svcGetSystemTick() - t0;
			if (++upCount == 600) {
				double up = (double) upTicks / upCount / CPU_TICKS_PER_MSEC;
				if (upTileTicks) {
					logLine("textures: a 240x160 picture's upload %.3f ms on average (tiled by the CPU: %.3f ms of it, the rest the frame start and the cache flush)", up,
					        (double) upTileTicks / upCount / CPU_TICKS_PER_MSEC);
				} else {
					const double per = (double) CPU_TICKS_PER_MSEC * upCount;
					static unsigned lastSerial;
					const unsigned frames = frameSerial - lastSerial;
					lastSerial = frameSerial;
					logLine("textures: a 240x160 picture's upload %.3f ms on average (tiled by the GPU, %s: swap %.3f, flush %.3f, transfer call %.3f)",
					        up, asyncUpload ? "queued in the frame" : "waited for", (double) upSwapTicks / per, (double) upFlushTicks8 / per,
					        (double) upXferTicks8 / per);
					logLine("textures: the GPU calls by part (ms a frame, %u frames, %u 240x160 uploads): frame begin %.3f, frame end %.3f, picture copies %.3f",
					        frames, (unsigned) upCount, (double) waitBegin / CPU_TICKS_PER_MSEC / (frames ? frames : 1),
					        (double) waitEnd / CPU_TICKS_PER_MSEC / (frames ? frames : 1), (double) waitCopy / CPU_TICKS_PER_MSEC / (frames ? frames : 1));
					waitBegin = waitEnd = waitCopy = 0;
					if (upTwinTicks) logLine("textures: the picture copied from the render worker's twin instead (no swap, no flush): call %.3f ms", (double) upTwinTicks / per);
					if (upRawTicks) logLine("textures: the picture copied as it is instead (raw_picture: no swap, no flush): call %.3f ms", (double) upRawTicks / per);
				}
				upTicks = upTileTicks = upSwapTicks = upFlushTicks8 = upXferTicks8 = upTwinTicks = upRawTicks = 0, upCount = 0;
			}
		}
		return done ? 0 : -1;
	}
	if (!rect->x && rect->w == t->w && !(rect->y & 7) && !(rect->h & 7) && rect->y + rect->h <= t->h &&
	    update_by_gpu(t, pixels, pitch, rect->y, rect->h)) {
		return 0;
	}
	return update_by_cpu(t, rect, pixels, pitch);
}

static int update_by_cpu(SDL_Texture* t, const SDL_Rect* rect, const void* pixels, int pitch) {
	u32* out = t->tex.data;
	u32 texW = t->tex.width;
	int step = t->w / t->tw + (t->w % t->tw ? 1 : 0); /* 1 unless the picture was too large */
	if (step < 1) step = 1;
	bool argb = t->format == SDL_PIXELFORMAT_ARGB8888;
	bool opaque = t->format == SDL_PIXELFORMAT_XBGR8888;
	if (step == 1 && rect->x == 0 && rect->w == t->tw && rect->y >= 0 && !(rect->y & 7) && rect->h > 0 && rect->y + rect->h <= t->th &&
	    (!(rect->h & 7) || rect->y + rect->h == t->th) &&
	    texW >= 64) {
		/* whole rows (whole bands of 8, or the picture's last): written
		 * straight into a plain buffer in the GPU's colour order, and the
		 * GPU's copy engine (the display transfer) lays them out in tiles,
		 * which took the CPU 35 ns a pixel. In a frame being drawn the transfer is queued in it (async_upload; the
		 * buffer is not written again in that frame), else by the slower way below */
		if (asyncUpload && !uploadSync) frame_begin();
		u32 h8 = (u32) (rect->h + 7) & ~7u;
		size_t need = (size_t) texW * h8 * 4;
		int k = stageNext;
		const bool usable = !inFrame || (asyncUpload && stagesFrame[k] != frameSerial);
		if (usable && need > stageSizes[k]) {
			if (stages[k]) linearFree(stages[k]);
			stages[k] = linearAlloc(need);
			stageSizes[k] = stages[k] ? need : 0;
		}
		u32* stage = usable ? stages[k] : NULL;
		if (stage) stageNext ^= 1;
		if (stage) {
			for (int y = 0; y < rect->h; ++y) {
				const Uint32* src = (const Uint32*) ((const Uint8*) pixels + (size_t) y * pitch);
				u32* o = stage + (size_t) y * texW;
				int x = 0;
				if (argb) for (; x < t->tw; ++x) o[x] = src[x] << 8 | src[x] >> 24;
				else if (opaque) for (; x < t->tw; ++x) o[x] = __builtin_bswap32(src[x] | 0xFF000000u);
				else for (; x < t->tw; ++x) o[x] = __builtin_bswap32(src[x]);
			}
			cache_flush(stage, (size_t) texW * (size_t) rect->h * 4);
			if (inFrame) stagesFrame[k] = frameSerial;
			C3D_SyncDisplayTransfer(stage, GX_BUFFER_DIM(texW, h8), out + (size_t) rect->y * texW, GX_BUFFER_DIM(texW, h8),
			                        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
			                        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
			                        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
			return 0;
		}
	}
	if (step == 1) {
		/* (the usual case: no division (the ARM11 has none: two a pixel
		 * were most of a 480x320 picture's 20 ms), each row's tiles worked
		 * out once) */
		static const u8 xbits[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
		int w = SDL_min(rect->w, t->tw - rect->x), h = SDL_min(rect->h, t->th - rect->y);
		for (int y = 0; y < h; ++y) {
			const Uint32* src = (const Uint32*) ((const Uint8*) pixels + (size_t) y * pitch);
			u32 ty = (u32) (rect->y + y);
			u32* row = out + ((((ty >> 3) * (texW >> 3)) << 6) | ((ty & 1) << 1) | ((ty & 2) << 2) | ((ty & 4) << 3));
			for (int x = 0; x < w; ++x) {
				u32 tx = (u32) (rect->x + x);
				Uint32 p = src[x];
				if (argb) p = p << 8 | p >> 24;
				else p = __builtin_bswap32(opaque ? p | 0xFF000000u : p);
				row[((tx >> 3) << 6) | xbits[tx & 7]] = p;
			}
		}
		GSPGPU_FlushDataCache(t->tex.data, t->tex.size);
		return 0;
	}
	for (int y = 0; y < rect->h; y += step) {
		const Uint32* src = (const Uint32*) ((const Uint8*) pixels + (size_t) y * pitch);
		u32 ty = (u32) ((rect->y + y) / step);
		if ((int) ty >= t->th) break;
		for (int x = 0; x < rect->w; x += step) {
			u32 tx = (u32) ((rect->x + x) / step);
			if ((int) tx >= t->tw) break;
			Uint32 p = src[x];
			/* the GPU's RGBA8 is R in the top byte */
			if (argb) p = p << 8 | p >> 24;
			else p = __builtin_bswap32(opaque ? p | 0xFF000000u : p);
			out[tiled(tx, ty, texW)] = p;
		}
	}
	cache_flush(t->tex.data, t->tex.size);
	return 0;
}

SDL_Texture* SDL_CreateTextureFromSurface(SDL_Renderer* r, SDL_Surface* s) {
	if (!s) return NULL;
	SDL_Texture* t = SDL_CreateTexture(r, s->format->format, SDL_TEXTUREACCESS_STATIC, s->w, s->h);
	if (!t) return NULL;
	t->blend = SDL_BLENDMODE_BLEND; /* (as SDL does for a surface with alpha) */
	SDL_UpdateTexture(t, NULL, s->pixels, s->pitch);
	return t;
}

int SDL_SetTextureBlendMode(SDL_Texture* t, SDL_BlendMode mode) {
	if (t) t->blend = mode;
	return 0;
}

int SDL_SetTextureScaleMode(SDL_Texture* t, SDL_ScaleMode mode) {
	if (t) t->scale = mode;
	return 0;
}

int SDL_SetTextureColorMod(SDL_Texture* t, Uint8 r, Uint8 g, Uint8 b) {
	if (t) t->r = r, t->g = g, t->b = b;
	return 0;
}

int SDL_SetTextureAlphaMod(SDL_Texture* t, Uint8 a) {
	if (t) t->a = a;
	return 0;
}

/* ------------------------------------------------------------ sound
 *
 * A queue, as SDL_QueueAudio's: the app puts in what the game made, and the
 * DSP plays it from a ring of wave buffers filled as they come free. */

#define WAVE_BUFFERS 6
#define WAVE_FRAMES 512
#define FIFO_FRAMES 16384

static bool dspUp, audioOpen, audioPaused;
static ndspWaveBuf waveBuf[WAVE_BUFFERS];
static s16* waveMem;
static s16 fifo[FIFO_FRAMES * 2];
static volatile u32 fifoHead, fifoCount; /* frames */
static LightLock audioLock;
static int deviceRate = 48000; /* what the app asked for */
static int sourceRate = 32768; /* what the game makes (SDL_NewAudioStream) */

/* Moves queued sound into wave buffers the DSP has finished. */
static void audio_feed(void) {
	for (int i = 0; i < WAVE_BUFFERS; ++i) {
		ndspWaveBuf* b = &waveBuf[i];
		if (b->status != NDSP_WBUF_FREE && b->status != NDSP_WBUF_DONE) continue;
		u32 n = fifoCount < WAVE_FRAMES ? fifoCount : WAVE_FRAMES;
		/* whole buffers while there's enough, so a short one is the last */
		if (n < WAVE_FRAMES / 2) break;
		s16* dst = b->data_pcm16;
		for (u32 k = 0; k < n; ++k) {
			u32 at = (fifoHead + k) % FIFO_FRAMES;
			dst[k * 2] = fifo[at * 2];
			dst[k * 2 + 1] = fifo[at * 2 + 1];
		}
		fifoHead = (fifoHead + n) % FIFO_FRAMES;
		fifoCount -= n;
		b->nsamples = n;
		cache_flush(dst, n * 4);
		ndspChnWaveBufAdd(0, b);
	}
}

static void audio_callback(void* arg) {
	(void) arg;
	if (!audioOpen || audioPaused) return;
	LightLock_Lock(&audioLock);
	audio_feed();
	LightLock_Unlock(&audioLock);
}

int SDL_AudioInit(const char* driver) {
	(void) driver;
	if (dspUp) return 0;
	/* ndsp needs the DSP firmware dump (sdmc:/3ds/dspfirm.cdc); without it
	 * the game runs silent. An emulator has no firmware to give, but ndsp
	 * won't start without one: there, and never on a console, a blank one. */
	Result rc = ndspInit();
	if (R_FAILED(rc) && isEmulator()) {
		static u8 blankFirmware[4096];
		ndspUseComponent(blankFirmware, sizeof(blankFirmware), 0xFF, 0xFF);
		rc = ndspInit();
	}
	if (R_FAILED(rc)) {
		set_error("ndspInit %08lX (no sdmc:/3ds/dspfirm.cdc?)", (unsigned long) rc);
		return -1;
	}
	LightLock_Init(&audioLock);
	dspUp = true;
	return 0;
}

void SDL_AudioQuit(void) {
	if (!dspUp) return;
	ndspExit();
	dspUp = false;
}

const char* SDL_GetCurrentAudioDriver(void) {
	return dspUp ? "ndsp" : NULL;
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int capture, const SDL_AudioSpec* want, SDL_AudioSpec* have, int changes) {
	(void) device;
	(void) capture;
	(void) changes;
	if (!dspUp && SDL_AudioInit(NULL) != 0) return 0;
	if (!waveMem) waveMem = linearAlloc(WAVE_BUFFERS * WAVE_FRAMES * 4);
	if (!waveMem) {
		set_error("no memory for sound");
		return 0;
	}
	deviceRate = want->freq > 0 ? want->freq : 48000;
	if (have) {
		*have = *want;
		have->freq = deviceRate;
		have->samples = WAVE_FRAMES;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspSetOutputCount(1);
	ndspChnReset(0);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, (float) sourceRate);
	float mix[12] = { 1.0f, 1.0f };
	ndspChnSetMix(0, mix);
	ndspChnWaveBufClear(0);
	memset(waveBuf, 0, sizeof(waveBuf));
	for (int i = 0; i < WAVE_BUFFERS; ++i) waveBuf[i].data_vaddr = waveMem + i * WAVE_FRAMES * 2;
	fifoHead = fifoCount = 0;
	audioPaused = true;
	audioOpen = true;
	ndspSetCallback(audio_callback, NULL);
	return 1;
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) {
	(void) dev;
	if (!audioOpen) return;
	audioOpen = false;
	ndspSetCallback(NULL, NULL);
	ndspChnWaveBufClear(0);
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause) {
	(void) dev;
	if (!audioOpen) return;
	audioPaused = pause != 0;
	ndspChnSetPaused(0, audioPaused);
}

int SDL_QueueAudio(SDL_AudioDeviceID dev, const void* data, Uint32 len) {
	(void) dev;
	if (!audioOpen) return -1;
	const s16* src = data;
	u32 frames = len / 4;
	LightLock_Lock(&audioLock);
	if (frames > FIFO_FRAMES - fifoCount) frames = FIFO_FRAMES - fifoCount;
	u32 tail = (fifoHead + fifoCount) % FIFO_FRAMES;
	for (u32 k = 0; k < frames; ++k) {
		u32 at = (tail + k) % FIFO_FRAMES;
		fifo[at * 2] = src[k * 2];
		fifo[at * 2 + 1] = src[k * 2 + 1];
	}
	fifoCount += frames;
	if (!audioPaused) audio_feed();
	LightLock_Unlock(&audioLock);
	return 0;
}

/* What hasn't been heard yet, in the bytes the app queued: its own rate's. */
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev) {
	(void) dev;
	if (!audioOpen) return 0;
	LightLock_Lock(&audioLock);
	u32 frames = fifoCount;
	for (int i = 0; i < WAVE_BUFFERS; ++i) {
		if (waveBuf[i].status == NDSP_WBUF_QUEUED) frames += waveBuf[i].nsamples;
		else if (waveBuf[i].status == NDSP_WBUF_PLAYING) {
			u32 pos = ndspChnGetSamplePos(0);
			frames += pos < waveBuf[i].nsamples ? waveBuf[i].nsamples - pos : 0;
		}
	}
	LightLock_Unlock(&audioLock);
	return (Uint32) ((u64) frames * deviceRate / sourceRate) * 4;
}

void SDL_ClearQueuedAudio(SDL_AudioDeviceID dev) {
	(void) dev;
	if (!audioOpen) return;
	LightLock_Lock(&audioLock);
	fifoHead = fifoCount = 0;
	ndspChnWaveBufClear(0);
	for (int i = 0; i < WAVE_BUFFERS; ++i) waveBuf[i].status = NDSP_WBUF_FREE;
	LightLock_Unlock(&audioLock);
}

struct SDL_AudioStream {
	s16 data[8192 * 2];
	int frames;
};

SDL_AudioStream* SDL_NewAudioStream(SDL_AudioFormat sf, Uint8 sc, int srate, SDL_AudioFormat df, Uint8 dc, int drate) {
	(void) sf;
	(void) sc;
	(void) df;
	(void) dc;
	(void) drate;
	SDL_AudioStream* s = calloc(1, sizeof(*s));
	if (!s) return NULL;
	if (srate > 0 && srate != sourceRate) {
		sourceRate = srate;
		if (audioOpen) ndspChnSetRate(0, (float) sourceRate);
	}
	return s;
}

int SDL_AudioStreamPut(SDL_AudioStream* s, const void* buf, int len) {
	int frames = len / 4;
	if (frames > 8192 - s->frames) frames = 8192 - s->frames;
	memcpy(s->data + s->frames * 2, buf, (size_t) frames * 4);
	s->frames += frames;
	return 0;
}

int SDL_AudioStreamGet(SDL_AudioStream* s, void* buf, int len) {
	int frames = len / 4;
	if (frames > s->frames) frames = s->frames;
	if (!frames) return 0;
	memcpy(buf, s->data, (size_t) frames * 4);
	s->frames -= frames;
	if (s->frames) memmove(s->data, s->data + frames * 2, (size_t) s->frames * 4);
	return frames * 4;
}

void SDL_FreeAudioStream(SDL_AudioStream* s) {
	free(s);
}

/* ------------------------------------------------------------ input
 *
 * One controller: the console's own buttons. A and B by position as SDL
 * names them (its A is the bottom face button: the 3DS's B), which is how
 * the app reads them; X and Y by their labels, which is what its menus and
 * hotkeys show. ZL is the controller's
 * GUIDE (GBA DS's menu button), ZR its right trigger; the Circle Pad is the
 * left stick. */

struct SDL_Joystick {
	int unused;
};
struct SDL_GameController {
	int unused;
};
static SDL_Joystick theJoystick;
static SDL_GameController thePad;
static u32 padHeld;
/* The buttons are read every few milliseconds on a thread of their own: the
 * app looks once a frame, and a frame that waits on the SD card (the game
 * list, while logos are made) is longer than a tap on the D-Pad. */
static atomic_uint padLatched, padNow;
static Thread padThread;
static volatile bool padStop;

static void pad_main(void* arg) {
	(void) arg;
	while (!padStop) {
		hidScanInput();
		atomic_fetch_or(&padLatched, hidKeysDown());
		atomic_store(&padNow, hidKeysHeld());
		svcSleepThread(4000000LL);
	}
}

static void pad_start(void) {
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	padStop = false;
	host_next_thread_name("pad");
	padThread = threadCreate(pad_main, NULL, 8 * 1024, priority - 1, host_side_core(), false);
}

static void pad_end(void) {
	if (!padThread) return;
	padStop = true;
	threadJoin(padThread, U64_MAX);
	threadFree(padThread);
	padThread = NULL;
}
static circlePosition circle;
static const u32 padKeys[SDL_CONTROLLER_BUTTON_MAX] = {
	[SDL_CONTROLLER_BUTTON_A] = KEY_B,
	[SDL_CONTROLLER_BUTTON_B] = KEY_A,
	[SDL_CONTROLLER_BUTTON_X] = KEY_X,
	[SDL_CONTROLLER_BUTTON_Y] = KEY_Y,
	[SDL_CONTROLLER_BUTTON_BACK] = KEY_SELECT,
	[SDL_CONTROLLER_BUTTON_GUIDE] = KEY_ZL,
	[SDL_CONTROLLER_BUTTON_START] = KEY_START,
	[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = KEY_L,
	[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = KEY_R,
	[SDL_CONTROLLER_BUTTON_DPAD_UP] = KEY_DUP,
	[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = KEY_DDOWN,
	[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = KEY_DLEFT,
	[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = KEY_DRIGHT,
};
static const char* const padNames[SDL_CONTROLLER_BUTTON_MAX] = {
	"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder", "rightshoulder",
	"dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad",
};

int SDL_NumJoysticks(void) {
	return 1;
}

SDL_bool SDL_IsGameController(int index) {
	return index == 0 ? SDL_TRUE : SDL_FALSE;
}

SDL_GameController* SDL_GameControllerOpen(int index) {
	return index == 0 ? &thePad : NULL;
}

const char* SDL_GameControllerName(SDL_GameController* c) {
	(void) c;
	return "Nintendo 3DS";
}

char* SDL_GameControllerMapping(SDL_GameController* c) {
	(void) c;
	return strdup("000000004e696e74656e646f20334453,Nintendo 3DS,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftshoulder:b9,"
	              "rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,righttrigger:a5,");
}

SDL_Joystick* SDL_GameControllerGetJoystick(SDL_GameController* c) {
	return c ? &theJoystick : NULL;
}

Uint8 SDL_GameControllerGetButton(SDL_GameController* c, SDL_GameControllerButton b) {
	(void) c;
	return b >= 0 && b < SDL_CONTROLLER_BUTTON_MAX && padKeys[b] && (padHeld & padKeys[b]) ? 1 : 0;
}

Sint16 SDL_GameControllerGetAxis(SDL_GameController* c, SDL_GameControllerAxis a) {
	(void) c;
	switch (a) {
	case SDL_CONTROLLER_AXIS_LEFTX: return (Sint16) SDL_clamp(circle.dx * 210, -32767, 32767);
	case SDL_CONTROLLER_AXIS_LEFTY: return (Sint16) SDL_clamp(-circle.dy * 210, -32767, 32767);
	case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return padHeld & KEY_ZR ? 32767 : 0;
	default: return 0;
	}
}

const char* SDL_GameControllerGetStringForButton(SDL_GameControllerButton b) {
	return b >= 0 && b < SDL_CONTROLLER_BUTTON_MAX ? padNames[b] : "?";
}

int SDL_GameControllerAddMapping(const char* mapping) {
	(void) mapping;
	return 0;
}

int SDL_GameControllerAddMappingsFromFile(const char* path) {
	(void) path;
	return 0;
}

SDL_Joystick* SDL_JoystickOpen(int index) {
	return index == 0 ? &theJoystick : NULL;
}

void SDL_JoystickClose(SDL_Joystick* j) {
	(void) j;
}

SDL_Joystick* SDL_JoystickFromInstanceID(SDL_JoystickID id) {
	return id == 0 ? &theJoystick : NULL;
}

const char* SDL_JoystickName(SDL_Joystick* j) {
	(void) j;
	return "Nintendo 3DS";
}

SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick* j) {
	(void) j;
	SDL_JoystickGUID g = { { 0, 0, 0, 0, 'N', 'i', 'n', 't', 'e', 'n', 'd', 'o', ' ', '3', 'D', 'S' } };
	return g;
}

void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char* out, int n) {
	int at = 0;
	for (int i = 0; i < 16 && at + 2 < n; ++i) at += snprintf(out + at, (size_t) (n - at), "%02x", guid.data[i]);
	if (n > 0) out[at < n ? at : n - 1] = 0;
}

SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int index) {
	return index == 0 ? 0 : -1;
}

int SDL_JoystickNumButtons(SDL_Joystick* j) {
	(void) j;
	return 15;
}

int SDL_JoystickNumAxes(SDL_Joystick* j) {
	(void) j;
	return 6;
}

int SDL_JoystickNumHats(SDL_Joystick* j) {
	(void) j;
	return 0;
}

Uint8 SDL_JoystickGetButton(SDL_Joystick* j, int b) {
	(void) j;
	return SDL_GameControllerGetButton(&thePad, (SDL_GameControllerButton) b);
}

Sint16 SDL_JoystickGetAxis(SDL_Joystick* j, int a) {
	(void) j;
	return SDL_GameControllerGetAxis(&thePad, (SDL_GameControllerAxis) a);
}

Uint8 SDL_JoystickGetHat(SDL_Joystick* j, int h) {
	(void) j;
	(void) h;
	return SDL_HAT_CENTERED;
}

void SDL_JoystickUpdate(void) {
}

int SDL_JoystickAttachVirtual(SDL_JoystickType type, int axes, int buttons, int hats) {
	(void) type;
	(void) axes;
	(void) buttons;
	(void) hats;
	set_error("no virtual controllers");
	return -1;
}

int SDL_JoystickSetVirtualAxis(SDL_Joystick* j, int axis, Sint16 v) {
	(void) j;
	(void) axis;
	(void) v;
	return -1;
}

int SDL_JoystickSetVirtualButton(SDL_Joystick* j, int button, Uint8 v) {
	(void) j;
	(void) button;
	(void) v;
	return -1;
}

int SDL_JoystickSetVirtualHat(SDL_Joystick* j, int hat, Uint8 v) {
	(void) j;
	(void) hat;
	(void) v;
	return -1;
}

const Uint8* SDL_GetKeyboardState(int* n) {
	static Uint8 none[SDL_NUM_SCANCODES];
	if (n) *n = SDL_NUM_SCANCODES;
	return none;
}

const char* SDL_GetKeyName(SDL_Keycode key) {
	(void) key;
	return "";
}

/* ------------------------------------------------------------ events */

#define EVENTS 64
static SDL_Event queue[EVENTS];
static int queueHead, queueCount;
static bool pumped;
static bool touching;
static touchPosition lastTouch;
static bool quitSent;

int SDL_PushEvent(SDL_Event* ev) {
	if (queueCount == EVENTS) return 0;
	queue[(queueHead + queueCount++) % EVENTS] = *ev;
	return 1;
}

static void push_finger(Uint32 type, const touchPosition* t) {
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.tfinger.x = (float) t->px / 320.0f;
	ev.tfinger.y = (float) t->py / 240.0f;
	ev.tfinger.windowID = 2; /* the bottom screen's window */
	SDL_PushEvent(&ev);
}

/* Once for each time the app empties the queue: the system's own events,
 * the buttons, the touch screen. */
static void pump(void) {
	if (!aptMainLoop()) {
		if (!quitSent) {
			SDL_Event ev;
			memset(&ev, 0, sizeof(ev));
			ev.type = SDL_QUIT;
			SDL_PushEvent(&ev);
			quitSent = true;
		}
		return;
	}
	{
		/* (a diagnosis: the frames that keep the buttons waiting) */
		static u64 last;
		static int told;
		u64 now = mono_ms();
		if (last && now - last > 150 && told < 200) ++told, logLine("slow frame: %u ms", (unsigned) (now - last));
		/* (osGetTime against the tick, a frame apart: shows the clock steps SDL_GetTicks avoids) */
		static u64 lastWall;
		static int stepsTold;
		u64 wall = osGetTime();
		if (last && lastWall && stepsTold < 20) {
			int drift = (int) ((int64_t) (wall - lastWall) - (int64_t) (now - last));
			if (drift > 5 || drift < -5) ++stepsTold, logLine("clock: osGetTime moved %+d ms against the tick in a frame of %u ms", drift, (unsigned) (now - last));
		}
		lastWall = wall;
		last = now;
		static unsigned pumps;
		if (++pumps % 900 == 0) host_mem_note("now");
	}
	u32 down;
	if (padThread) {
		/* (a press shorter than the frame still counts as held for this one) */
		down = atomic_exchange(&padLatched, 0);
		if (down) host_ui_busy();
		padHeld = atomic_load(&padNow) | down;
	} else {
		hidScanInput();
		down = hidKeysDown();
		padHeld = hidKeysHeld();
	}
	hidCircleRead(&circle);
	for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) {
		if (!padKeys[b] || !(down & padKeys[b])) continue;
		SDL_Event ev;
		memset(&ev, 0, sizeof(ev));
		ev.type = SDL_JOYBUTTONDOWN;
		ev.jbutton.button = (Uint8) b;
		ev.jbutton.state = 1;
		SDL_PushEvent(&ev);
		ev.type = SDL_CONTROLLERBUTTONDOWN;
		SDL_PushEvent(&ev);
	}
	if (padHeld & KEY_TOUCH) {
		touchPosition t;
		hidTouchRead(&t);
		if (!touching) push_finger(SDL_FINGERDOWN, &t);
		else if (t.px != lastTouch.px || t.py != lastTouch.py) push_finger(SDL_FINGERMOTION, &t);
		lastTouch = t;
		touching = true;
	} else if (touching) {
		push_finger(SDL_FINGERUP, &lastTouch);
		touching = false;
	}
}

int SDL_PollEvent(SDL_Event* ev) {
	if (!queueCount && !pumped) {
		pump();
		pumped = true;
	}
	if (!queueCount) {
		pumped = false;
		return 0;
	}
	if (ev) *ev = queue[queueHead];
	queueHead = (queueHead + 1) % EVENTS;
	--queueCount;
	return 1;
}

/* ------------------------------------------------------------ start, end */

int SDL_Init(Uint32 flags) {
	mono_ms();
	if ((flags & SDL_INIT_VIDEO) && !videoUp) {
		logLine("start: screens");
		gfxInitDefault();
		if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) || !C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
			set_error("the GPU didn't start");
			return -1;
		}
		C2D_Prepare();
		ptmuInit();
		videoUp = true;
		logLine("start: the GPU is up");
		tiling_selftest();
		cpu_tile_selftest();
		twin_selftest();
		if (!padThread) pad_start();
	}
	if (flags & SDL_INIT_AUDIO) {
		int rc = SDL_AudioInit(NULL); /* (silent without the DSP's firmware) */
		logLine("start: sound %s", rc == 0 ? "is up" : SDL_GetError());
	}
	return 0;
}

void SDL_Quit(void) {
	pad_end();
	SDL_CloseAudioDevice(1);
	SDL_AudioQuit();
	if (videoUp) {
		if (inFrame) C3D_FrameEnd(0);
		inFrame = false;
		ptmuExit();
		C2D_Fini();
		C3D_Fini();
		gfxExit();
		videoUp = false;
	}
}
