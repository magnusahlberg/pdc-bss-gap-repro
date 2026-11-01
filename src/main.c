// Minimal repro: pdc reserves too little memory for a game whose .bss is more
// aligned than the end of its .data.
//
// The SDK linker script (C_API/buildsupport/link_map.ld) ends .data on a
// 4-byte boundary. A 16-byte-aligned object in .bss makes the linker start
// .bss on a 16-byte boundary, leaving a gap after .data. pdc writes the
// game's memory size into the pdex.bin header as file size + .bss size,
// which leaves the gap out, so the last bytes of .bss lie past the memory
// the OS reserves for the game, where the heap lives.
//
// This program reads its own pdex.bin header, shows how many bytes of .bss
// lie outside the reserved memory, and checks whether the values stored
// there survive ordinary heap use.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"

// Initialized words, to set where .data ends. With SDK 3.1.1 one word gives a
// 12-byte gap before .bss (the largest possible); `make layout` prints it.
#ifndef DATA_WORDS
#define DATA_WORDS 1
#endif
volatile uint32_t data_pad[DATA_WORDS] = { 1 };

// All of this file's .bss in one object, so its layout is fixed: 16-byte
// aligned (as a cglm mat4 or any SIMD type would be), canaries last. The
// only .bss after it is setup.c's pdrealloc, the last word of .bss.
#define CANARY_COUNT 8
#define LINE_COUNT 12
static struct {
	float matrix[4] __attribute__((aligned(16)));
	PlaydateAPI* pd;
	LCDFont* font;
	int lineCount;
	char lines[LINE_COUNT][64];
	uint32_t canary[CANARY_COUNT];
} g;

#define CANARY(i) (0xC0DE0000u | (uint32_t)(i))

#if TARGET_PLAYDATE
extern uint8_t __data_end__[], __bss_start__[], __bss_end__[];
extern int eventHandlerShim(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg);

// setup.c's pdrealloc is static; it is the last word of .bss
#define PDREALLOC_SLOT (*(void**)(__bss_end__ - sizeof(void*)))
#endif

static void addLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void addLine(const char* fmt, ...) {
	if (g.lineCount >= LINE_COUNT) return;
	va_list a;
	va_start(a, fmt);
	vsnprintf(g.lines[g.lineCount], sizeof g.lines[0], fmt, a);
	va_end(a);
	g.pd->system->logToConsole("%s", g.lines[g.lineCount]);
	g.lineCount++;
}

#if TARGET_PLAYDATE
static uintptr_t reservedEnd(void) {
	// pdex.bin header: 0x20 file size, 0x24 memory size, 0x28 entry point
	// (offsets from the load address)
	uint32_t header[3] = { 0 };
	SDFile* f = g.pd->file->open("pdex.bin", kFileRead);
	int ok = f != NULL && g.pd->file->seek(f, 0x20, SEEK_SET) == 0 &&
		g.pd->file->read(f, header, sizeof header) == (int)sizeof header;
	if (f) g.pd->file->close(f);
	if (!ok) return 0;

	uintptr_t base = (uintptr_t)eventHandlerShim - header[2];
	addLine("pdex.bin: file %lu, memory %lu bytes", (unsigned long)header[0], (unsigned long)header[1]);
	return base + header[1];
}
#endif

static void report(void* realloc_) {
#if TARGET_PLAYDATE
	uintptr_t dataEnd = (uintptr_t)__data_end__;
	uintptr_t bssStart = (uintptr_t)__bss_start__;
	uintptr_t bssEnd = (uintptr_t)__bss_end__;
	uintptr_t resEnd = reservedEnd();
	addLine(".data ends    0x%08lx", (unsigned long)dataEnd);
	addLine(".bss starts   0x%08lx (gap %ld)", (unsigned long)bssStart, (long)(bssStart - dataEnd));
	addLine(".bss ends     0x%08lx", (unsigned long)bssEnd);
	addLine("reserved ends 0x%08lx", (unsigned long)resEnd);
	addLine("=> %ld bytes of .bss outside it", (long)(bssEnd - resEnd));
#else
	addLine("Run on a device (the Simulator uses pdex.dylib)");
#endif

	int bad = 0;
	for (int i = 0; i < CANARY_COUNT; i++) {
		if (g.canary[i] != CANARY(i)) {
			if (bad < 2)
				addLine("canary[%d] @0x%08lx = 0x%08lx", i, (unsigned long)(uintptr_t)&g.canary[i], (unsigned long)g.canary[i]);
			bad++;
		}
	}
#if TARGET_PLAYDATE
	void* slot = PDREALLOC_SLOT;
	addLine("pdrealloc %s (0x%08lx)", slot == realloc_ ? "intact" : "OVERWRITTEN", (unsigned long)(uintptr_t)slot);
#endif
	addLine(bad ? "FAIL: %d of %d canaries overwritten" : "canaries intact (%d of %d bad)", bad, CANARY_COUNT);
}

static int update(void* userdata) {
	(void)userdata;
	g.pd->graphics->clear(kColorWhite);
	g.pd->graphics->setFont(g.font);
	for (int i = 0; i < g.lineCount; i++)
		g.pd->graphics->drawText(g.lines[i], strlen(g.lines[i]), kASCIIEncoding, 6, 4 + i * 19);
	return 1;
}

int eventHandler(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg) {
	(void)arg;
	if (event == kEventInit) {
		g.pd = pd;
		g.matrix[0] = (float)data_pad[0];
		for (int i = 0; i < CANARY_COUNT; i++)
			g.canary[i] = CANARY(i);

		// Ordinary heap use by the system and by the game. Our malloc goes
		// through pdrealloc, so skip it if pdrealloc is already gone.
		const char* err = NULL;
		g.font = pd->graphics->loadFont("/System/Fonts/Asheville-Sans-14-Bold.pft", &err);
#if TARGET_PLAYDATE
		if (PDREALLOC_SLOT == (void*)pd->system->realloc)
#endif
		{
			for (int i = 0; i < 32; i++) {
				void* block = malloc(256);
				if (block) memset(block, 0x5A, 256);
			}
		}

		report((void*)pd->system->realloc);
		pd->system->setUpdateCallback(update, NULL);
	}
	return 0;
}
