#include <ctype.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gba.h"
#include "../globals.h"
#include "../memory.h"
#include "../port.h"
#include "../sound.h"
#include "../system.h"
#include "../types.h"

#include "ui.h"

#include <switch.h>

uint8_t libretro_save_buf[0x20000 + 0x2000]; /* Workaround for broken-by-design GBA save semantics. */

extern uint64_t joy;
static bool can_dupe;
unsigned device_type = 0;

static bool emulationRunning = false;
static bool emulationPaused = false;

static char currentRomPath[PATH_LENGTH] = {'\0'};

static Mutex videoLock;
static u16 *videoTransferBuffer;

static Mutex inputLock;
static u32 inputTransferKeysHeld;

static Mutex emulationLock;

static bool running = true;

char filename_bios[0x100] = {0};

#define AUDIO_SAMPLERATE 48000
#define AUDIO_BUFFER_COUNT 6
#define AUDIO_BUFFER_SAMPLES (AUDIO_SAMPLERATE / 20)

static u32 audioBufferSize;

/*typedef struct {
	bool enqueued;
	AudioOutBuffer buffer;
} QueueAudioBuffer;

static QueueAudioBuffer audioBuffer[AUDIO_BUFFER_COUNT];
static QueueAudioBuffer *audioBufferQueue[AUDIO_BUFFER_COUNT];
static int audioBufferQueueNext = 0;*/

static AudioOutBuffer audioBuffer[AUDIO_BUFFER_COUNT];

static unsigned libretro_save_size = sizeof(libretro_save_buf);

const char *frameSkipNames[] = {"No Frameskip", "1/3", "1/2", "1", "2", "3", "4"};
const int frameSkipValues[] = {0, 0x13, 0x12, 0x1, 0x2, 0x3, 0x4};

static int frameSkip = 0;

int buttonMap[10] = {KEY_A, KEY_B, KEY_MINUS, KEY_PLUS, KEY_RIGHT, KEY_LEFT, KEY_UP, KEY_DOWN, KEY_R, KEY_L};

static bool scan_area(const uint8_t *data, unsigned size) {
	for (unsigned i = 0; i < size; i++)
		if (data[i] != 0xff) return true;

	return false;
}

static void adjust_save_ram() {
	if (scan_area(libretro_save_buf, 512) && !scan_area(libretro_save_buf + 512, sizeof(libretro_save_buf) - 512)) {
		libretro_save_size = 512;
		printf("Detecting EEprom 8kbit\n");
	} else if (scan_area(libretro_save_buf, 0x2000) && !scan_area(libretro_save_buf + 0x2000, sizeof(libretro_save_buf) - 0x2000)) {
		libretro_save_size = 0x2000;
		printf("Detecting EEprom 64kbit\n");
	}

	else if (scan_area(libretro_save_buf, 0x10000) && !scan_area(libretro_save_buf + 0x10000, sizeof(libretro_save_buf) - 0x10000)) {
		libretro_save_size = 0x10000;
		printf("Detecting Flash 512kbit\n");
	} else if (scan_area(libretro_save_buf, 0x20000) && !scan_area(libretro_save_buf + 0x20000, sizeof(libretro_save_buf) - 0x20000)) {
		libretro_save_size = 0x20000;
		printf("Detecting Flash 1Mbit\n");
	} else
		printf("Did not detect any particular SRAM type.\n");

	if (libretro_save_size == 512 || libretro_save_size == 0x2000)
		eepromData = libretro_save_buf;
	else if (libretro_save_size == 0x10000 || libretro_save_size == 0x20000)
		flashSaveMemory = libretro_save_buf;
}

void romPathWithExt(char *out, const char *ext) {
	strcpy(out, currentRomPath);
	int dotLoc = strlen(out);
	while (dotLoc >= 0 && out[dotLoc] != '.') dotLoc--;

	int extLen = strlen(ext);
	for (int i = 0; i < extLen + 1; i++) out[dotLoc + 1 + i] = ext[i];
}

void retro_init(void) {
	memset(libretro_save_buf, 0xff, sizeof(libretro_save_buf));
	adjust_save_ram();
#if HAVE_HLE_BIOS
	const char *dir = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir) {
		strncpy(filename_bios, dir, sizeof(filename_bios));
		strncat(filename_bios, "/gba_bios.bin", sizeof(filename_bios));
	}
#endif

#ifdef FRONTEND_SUPPORTS_RGB565
	enum retro_pixel_format rgb565 = RETRO_PIXEL_FORMAT_RGB565;
	if (environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565) && log_cb)
		log_cb(RETRO_LOG_INFO, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");
#endif

#if THREADED_RENDERER
	ThreadedRendererStart();
#endif
}

static unsigned serialize_size = 0;

typedef struct {
	char romtitle[256];
	char romid[5];
	int flashSize;
	int saveType;
	int rtcEnabled;
	int mirroringEnabled;
	int useBios;
} ini_t;

static const ini_t gbaover[256] = {
    // romtitle,							    	romid	flash	save	rtc	mirror	bios
    {"2 Games in 1 - Dragon Ball Z - The Legacy of Goku I & II (USA)", "BLFE", 0, 1, 0, 0, 0},
    {"2 Games in 1 - Dragon Ball Z - Buu's Fury + Dragon Ball GT - Transformation (USA)", "BUFE", 0, 1, 0, 0, 0},
    {"Boktai - The Sun Is in Your Hand (Europe)(En,Fr,De,Es,It)", "U3IP", 0, 0, 1, 0, 0},
    {"Boktai - The Sun Is in Your Hand (USA)", "U3IE", 0, 0, 1, 0, 0},
    {"Boktai 2 - Solar Boy Django (USA)", "U32E", 0, 0, 1, 0, 0},
    {"Boktai 2 - Solar Boy Django (Europe)(En,Fr,De,Es,It)", "U32P", 0, 0, 1, 0, 0},
    {"Bokura no Taiyou - Taiyou Action RPG (Japan)", "U3IJ", 0, 0, 1, 0, 0},
    {"Card e-Reader+ (Japan)", "PSAJ", 131072, 0, 0, 0, 0},
    {"Classic NES Series - Bomberman (USA, Europe)", "FBME", 0, 1, 0, 1, 0},
    {"Classic NES Series - Castlevania (USA, Europe)", "FADE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Donkey Kong (USA, Europe)", "FDKE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Dr. Mario (USA, Europe)", "FDME", 0, 1, 0, 1, 0},
    {"Classic NES Series - Excitebike (USA, Europe)", "FEBE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Legend of Zelda (USA, Europe)", "FZLE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Ice Climber (USA, Europe)", "FICE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Metroid (USA, Europe)", "FMRE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Pac-Man (USA, Europe)", "FP7E", 0, 1, 0, 1, 0},
    {"Classic NES Series - Super Mario Bros. (USA, Europe)", "FSME", 0, 1, 0, 1, 0},
    {"Classic NES Series - Xevious (USA, Europe)", "FXVE", 0, 1, 0, 1, 0},
    {"Classic NES Series - Zelda II - The Adventure of Link (USA, Europe)", "FLBE", 0, 1, 0, 1, 0},
    {"Digi Communication 2 - Datou! Black Gemagema Dan (Japan)", "BDKJ", 0, 1, 0, 0, 0},
    {"e-Reader (USA)", "PSAE", 131072, 0, 0, 0, 0},
    {"Dragon Ball GT - Transformation (USA)", "BT4E", 0, 1, 0, 0, 0},
    {"Dragon Ball Z - Buu's Fury (USA)", "BG3E", 0, 1, 0, 0, 0},
    {"Dragon Ball Z - Taiketsu (Europe)(En,Fr,De,Es,It)", "BDBP", 0, 1, 0, 0, 0},
    {"Dragon Ball Z - Taiketsu (USA)", "BDBE", 0, 1, 0, 0, 0},
    {"Dragon Ball Z - The Legacy of Goku II International (Japan)", "ALFJ", 0, 1, 0, 0, 0},
    {"Dragon Ball Z - The Legacy of Goku II (Europe)(En,Fr,De,Es,It)", "ALFP", 0, 1, 0, 0, 0},
    {"Dragon Ball Z - The Legacy of Goku II (USA)", "ALFE", 0, 1, 0, 0, 0},
    {"Dragon Ball Z - The Legacy Of Goku (Europe)(En,Fr,De,Es,It)", "ALGP", 0, 1, 0, 0, 0},
    {"Dragon Ball Z - The Legacy of Goku (USA)", "ALGE", 131072, 1, 0, 0, 0},
    {"F-Zero - Climax (Japan)", "BFTJ", 131072, 0, 0, 0, 0},
    {"Famicom Mini Vol. 01 - Super Mario Bros. (Japan)", "FMBJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 12 - Clu Clu Land (Japan)", "FCLJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 13 - Balloon Fight (Japan)", "FBFJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 14 - Wrecking Crew (Japan)", "FWCJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 15 - Dr. Mario (Japan)", "FDMJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 16 - Dig Dug (Japan)", "FTBJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 17 - Takahashi Meijin no Boukenjima (Japan)", "FTBJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 18 - Makaimura (Japan)", "FMKJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 19 - Twin Bee (Japan)", "FTWJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 20 - Ganbare Goemon! Karakuri Douchuu (Japan)", "FGGJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 21 - Super Mario Bros. 2 (Japan)", "FM2J", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 22 - Nazo no Murasame Jou (Japan)", "FNMJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 23 - Metroid (Japan)", "FMRJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 24 - Hikari Shinwa - Palthena no Kagami (Japan)", "FPTJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 25 - The Legend of Zelda 2 - Link no Bouken (Japan)", "FLBJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 26 - Famicom Mukashi Banashi - Shin Onigashima - Zen Kou Hen (Japan)", "FFMJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 27 - Famicom Tantei Club - Kieta Koukeisha - Zen Kou Hen (Japan)", "FTKJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 28 - Famicom Tantei Club Part II - Ushiro ni Tatsu Shoujo - Zen Kou Hen (Japan)", "FTUJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 29 - Akumajou Dracula (Japan)", "FADJ", 0, 1, 0, 1, 0},
    {"Famicom Mini Vol. 30 - SD Gundam World - Gachapon Senshi Scramble Wars (Japan)", "FSDJ", 0, 1, 0, 1, 0},
    {"Game Boy Wars Advance 1+2 (Japan)", "BGWJ", 131072, 0, 0, 0, 0},
    {"Golden Sun - The Lost Age (USA)", "AGFE", 65536, 0, 0, 1, 0},
    {"Golden Sun (USA)", "AGSE", 65536, 0, 0, 1, 0},
    {"Iridion II (Europe) (En,Fr,De)", "AI2P", 0, 5, 0, 0, 0},
    {"Iridion II (USA)", "AI2E", 0, 5, 0, 0, 0},
    {"Koro Koro Puzzle - Happy Panechu! (Japan)", "KHPJ", 0, 4, 0, 0, 0},
    {"Mario vs. Donkey Kong (Europe)", "BM5P", 0, 3, 0, 0, 0},
    {"Pocket Monsters - Emerald (Japan)", "BPEJ", 131072, 0, 1, 0, 0},
    {"Pocket Monsters - Fire Red (Japan)", "BPRJ", 131072, 0, 0, 0, 0},
    {"Pocket Monsters - Leaf Green (Japan)", "BPGJ", 131072, 0, 0, 0, 0},
    {"Pocket Monsters - Ruby (Japan)", "AXVJ", 131072, 0, 1, 0, 0},
    {"Pocket Monsters - Sapphire (Japan)", "AXPJ", 131072, 0, 1, 0, 0},
    {"Pokemon Mystery Dungeon - Red Rescue Team (USA, Australia)", "B24E", 131072, 0, 0, 0, 0},
    {"Pokemon Mystery Dungeon - Red Rescue Team (En,Fr,De,Es,It)", "B24P", 131072, 0, 0, 0, 0},
    {"Pokemon - Blattgruene Edition (Germany)", "BPGD", 131072, 0, 0, 0, 0},
    {"Pokemon - Edicion Rubi (Spain)", "AXVS", 131072, 0, 1, 0, 0},
    {"Pokemon - Edicion Esmeralda (Spain)", "BPES", 131072, 0, 1, 0, 0},
    {"Pokemon - Edicion Rojo Fuego (Spain)", "BPRS", 131072, 1, 0, 0, 0},
    {"Pokemon - Edicion Verde Hoja (Spain)", "BPGS", 131072, 1, 0, 0, 0},
    {"Pokemon - Eidicion Zafiro (Spain)", "AXPS", 131072, 0, 1, 0, 0},
    {"Pokemon - Emerald Version (USA, Europe)", "BPEE", 131072, 0, 1, 0, 0},
    {"Pokemon - Feuerrote Edition (Germany)", "BPRD", 131072, 0, 0, 0, 0},
    {"Pokemon - Fire Red Version (USA, Europe)", "BPRE", 131072, 0, 0, 0, 0},
    {"Pokemon - Leaf Green Version (USA, Europe)", "BPGE", 131072, 0, 0, 0, 0},
    {"Pokemon - Rubin Edition (Germany)", "AXVD", 131072, 0, 1, 0, 0},
    {"Pokemon - Ruby Version (USA, Europe)", "AXVE", 131072, 0, 1, 0, 0},
    {"Pokemon - Sapphire Version (USA, Europe)", "AXPE", 131072, 0, 1, 0, 0},
    {"Pokemon - Saphir Edition (Germany)", "AXPD", 131072, 0, 1, 0, 0},
    {"Pokemon - Smaragd Edition (Germany)", "BPED", 131072, 0, 1, 0, 0},
    {"Pokemon - Version Emeraude (France)", "BPEF", 131072, 0, 1, 0, 0},
    {"Pokemon - Version Rouge Feu (France)", "BPRF", 131072, 0, 0, 0, 0},
    {"Pokemon - Version Rubis (France)", "AXVF", 131072, 0, 1, 0, 0},
    {"Pokemon - Version Saphir (France)", "AXPF", 131072, 0, 1, 0, 0},
    {"Pokemon - Version Vert Feuille (France)", "BPGF", 131072, 0, 0, 0, 0},
    {"Pokemon - Versione Rubino (Italy)", "AXVI", 131072, 0, 1, 0, 0},
    {"Pokemon - Versione Rosso Fuoco (Italy)", "BPRI", 131072, 0, 0, 0, 0},
    {"Pokemon - Versione Smeraldo (Italy)", "BPEI", 131072, 0, 1, 0, 0},
    {"Pokemon - Versione Verde Foglia (Italy)", "BPGI", 131072, 0, 0, 0, 0},
    {"Pokemon - Versione Zaffiro (Italy)", "AXPI", 131072, 0, 1, 0, 0},
    {"Rockman EXE 4.5 - Real Operation (Japan)", "BR4J", 0, 0, 1, 0, 0},
    {"Rocky (Europe)(En,Fr,De,Es,It)", "AROP", 0, 1, 0, 0, 0},
    {"Rocky (USA)(En,Fr,De,Es,It)", "AR8e", 0, 1, 0, 0, 0},
    {"Sennen Kazoku (Japan)", "BKAJ", 131072, 0, 1, 0, 0},
    {"Shin Bokura no Taiyou - Gyakushuu no Sabata (Japan)", "U33J", 0, 1, 1, 0, 0},
    {"Super Mario Advance 4 (Japan)", "AX4J", 131072, 0, 0, 0, 0},
    {"Super Mario Advance 4 - Super Mario Bros. 3 (Europe)(En,Fr,De,Es,It)", "AX4P", 131072, 0, 0, 0, 0},
    {"Super Mario Advance 4 - Super Mario Bros 3 - Super Mario Advance 4 v1.1 (USA)", "AX4E", 131072, 0, 0, 0, 0},
    {"Top Gun - Combat Zones (USA)(En,Fr,De,Es,It)", "A2YE", 0, 5, 0, 0, 0},
    {"Yoshi's Universal Gravitation (Europe)(En,Fr,De,Es,It)", "KYGP", 0, 4, 0, 0, 0},
    {"Yoshi no Banyuuinryoku (Japan)", "KYGJ", 0, 4, 0, 0, 0},
    {"Yoshi - Topsy-Turvy (USA)", "KYGE", 0, 1, 0, 0, 0},
    {"Yu-Gi-Oh! GX - Duel Academy (USA)", "BYGE", 0, 2, 0, 0, 1},
    {"Yu-Gi-Oh! - Ultimate Masters - 2006 (Europe)(En,Jp,Fr,De,Es,It)", "BY6P", 0, 2, 0, 0, 0},
    {"Zoku Bokura no Taiyou - Taiyou Shounen Django (Japan)", "U32J", 0, 0, 1, 0, 0}};

static void load_image_preferences(void) {
	char buffer[5];
	buffer[0] = rom[0xac];
	buffer[1] = rom[0xad];
	buffer[2] = rom[0xae];
	buffer[3] = rom[0xaf];
	buffer[4] = 0;
	printf("GameID in ROM is: %s\n", buffer);

	bool found = false;
	int found_no = 0;

	for (int i = 0; i < 256; i++) {
		if (!strcmp(gbaover[i].romid, buffer)) {
			found = true;
			found_no = i;
			break;
		}
	}

	if (found) {
		printf("Found ROM in vba-over list.\n");

		enableRtc = gbaover[found_no].rtcEnabled;

		if (gbaover[found_no].flashSize != 0)
			flashSize = gbaover[found_no].flashSize;
		else
			flashSize = 65536;

		cpuSaveType = gbaover[found_no].saveType;

		mirroringEnable = gbaover[found_no].mirroringEnabled;
	}

	printf("RTC = %d.\n", enableRtc);
	printf("flashSize = %d.\n", flashSize);
	printf("cpuSaveType = %d.\n", cpuSaveType);
	printf("mirroringEnable = %d.\n", mirroringEnable);
}

static void gba_init(void) {
	cpuSaveType = 0;
	flashSize = 0x10000;
	enableRtc = false;
	mirroringEnable = false;

	load_image_preferences();

	if (flashSize == 0x10000 || flashSize == 0x20000) flashSetSize(flashSize);

	if (enableRtc) rtcEnable(enableRtc);

	doMirroring(mirroringEnable);

	soundSetSampleRate(AUDIO_SAMPLERATE + 20);

#if HAVE_HLE_BIOS
	bool usebios = false;

	struct retro_variable var;

	var.key = "vbanext_bios";
	var.value = NULL;

	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "disabled") == 0)
			usebios = false;
		else if (strcmp(var.value, "enabled") == 0)
			usebios = true;
	}

	if (usebios && filename_bios[0])
		CPUInit(filename_bios, true);
	else
		CPUInit(NULL, false);
#else
	CPUInit(NULL, false);
#endif
	CPUReset();

	soundReset();

	uint8_t *state_buf = (uint8_t *)malloc(2000000);
	serialize_size = CPUWriteState(state_buf, 2000000);
	free(state_buf);
}

void retro_deinit(void) {
#if THREADED_RENDERER
	ThreadedRendererStop();
#endif

	CPUCleanUp();
}

void retro_reset(void) { CPUReset(); }
/*
static const unsigned binds[] = {RETRO_DEVICE_ID_JOYPAD_A,     RETRO_DEVICE_ID_JOYPAD_B,     RETRO_DEVICE_ID_JOYPAD_SELECT,
				 RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT,
				 RETRO_DEVICE_ID_JOYPAD_UP,    RETRO_DEVICE_ID_JOYPAD_DOWN,  RETRO_DEVICE_ID_JOYPAD_R,
				 RETRO_DEVICE_ID_JOYPAD_L};

static const unsigned binds2[] = {RETRO_DEVICE_ID_JOYPAD_B,     RETRO_DEVICE_ID_JOYPAD_A,     RETRO_DEVICE_ID_JOYPAD_SELECT,
				  RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT,
				  RETRO_DEVICE_ID_JOYPAD_UP,    RETRO_DEVICE_ID_JOYPAD_DOWN,  RETRO_DEVICE_ID_JOYPAD_R,
				  RETRO_DEVICE_ID_JOYPAD_L};*/

static bool has_video_frame, has_audio_frame;

void pause_emulation() {
	mutexLock(&emulationLock);
	emulationPaused = true;
	uiSetState(statePaused);
	mutexUnlock(&emulationLock);
}
void unpause_emulation() {
	mutexLock(&emulationLock);
	emulationPaused = false;
	uiSetState(stateRunning);
	mutexUnlock(&emulationLock);
}

void retro_run() {
	mutexLock(&inputLock);
	joy = 0;
	for (unsigned i = 0; i < 10; i++) {
		joy |= ((bool)(inputTransferKeysHeld & buttonMap[i])) << i;
	}
	mutexUnlock(&inputLock);

	has_video_frame = false;
	has_audio_frame = 0;
	UpdateJoypad();
	do {
		CPULoop();
	} while (!has_video_frame || !has_audio_frame);
}

bool retro_load_game() {
	int ret = CPULoadRom(currentRomPath);

	gba_init();

	char saveFileName[PATH_LENGTH];
	romPathWithExt(saveFileName, "sav");
	if (CPUReadBatteryFile(saveFileName)) uiStatusMsg("loaded savefile %s", saveFileName);

	return ret;
}

static unsigned g_audio_frames = 0;
static unsigned g_video_frames = 0;

void retro_unload_game(void) {
	printf("[VBA] Sync stats: Audio frames: %u, Video frames: %u, AF/VF: %.2f\n", g_audio_frames, g_video_frames,
	       (float)g_audio_frames / g_video_frames);
	g_audio_frames = 0;
	g_video_frames = 0;

	char saveFilename[PATH_LENGTH];
	romPathWithExt(saveFilename, "sav");
	if (CPUWriteBatteryFile(saveFilename)) uiStatusMsg("wrote savefile %s", saveFilename);
}

void systemOnWriteDataToSoundBuffer(int16_t *finalWave, int length) {
	for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
		bool contains;
		if (/*!audioBuffer[i].enqueued && */R_SUCCEEDED(audoutContainsAudioOutBuffer(&audioBuffer[i], &contains)) && !contains) {
			AudioOutBuffer *outBuffer = &audioBuffer[i];
			memcpy(outBuffer->buffer, finalWave, length * sizeof(u16));

			outBuffer->data_size = length * sizeof(u16);

			/*audioBuffer[i].enqueued = true;

			audioBufferQueue[audioBufferQueueNext++] = &audioBuffer[i];*/

			break;
		}
	}

	AudioOutBuffer *releasedBuffer[AUDIO_BUFFER_COUNT];
	u32 releasedCount;
	audoutGetReleasedAudioOutBuffer(&releasedBuffer[0], &releasedCount);

	has_audio_frame = true;

	g_audio_frames += length / 2;
}

u32 bgr_555_to_rgb_888_table[32 * 32 * 32];
void init_color_lut() {
	for (u8 r5 = 0; r5 < 32; r5++) {
		for (u8 g5 = 0; g5 < 32; g5++) {
			for (u8 b5 = 0; b5 < 32; b5++) {
				u8 r8 = (u8)((r5 * 527 + 23) >> 6);
				u8 g8 = (u8)((g5 * 527 + 23) >> 6);
				u8 b8 = (u8)((b5 * 527 + 23) >> 6);
				u32 bgr555 = (r5 << 10) | (g5 << 5) | b5;
				u32 bgr888 = r8 | (g8 << 8) | (b8 << 16) | (255 << 24);
				bgr_555_to_rgb_888_table[bgr555] = bgr888;
			}
		}
	}
}

void systemDrawScreen() {
	mutexLock(&videoLock);
	memcpy(videoTransferBuffer, pix, sizeof(u16) * 256 * 160);
	mutexUnlock(&videoLock);

	g_video_frames++;
	has_video_frame = true;
}

void systemMessage(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

void threadFunc(void *args) {
	mutexLock(&emulationLock);
	retro_init();
	mutexUnlock(&emulationLock);
	init_color_lut();

	uiInit();
	uiSetState(stateFileselect);

	while (running) {
#define SECONDS_PER_TICKS (1.0 / 19200000)
#define TARGET_FRAMETIME (1.0 / 59.8260982880808)
		double startTime = (double)svcGetSystemTick() * SECONDS_PER_TICKS;

		mutexLock(&emulationLock);

		if (emulationRunning && !emulationPaused) retro_run();

		mutexUnlock(&emulationLock);

		double endTime = (double)svcGetSystemTick() * SECONDS_PER_TICKS;

		if (endTime - startTime < TARGET_FRAMETIME && !(inputTransferKeysHeld & KEY_ZL)) {
			svcSleepThread((u64)fabs((TARGET_FRAMETIME - (endTime - startTime)) * 1000000000 - 100));
		}

		/*int audioBuffersPlaying = 0;
		for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
			bool contained;
			if (R_SUCCEEDED(audoutContainsAudioOutBuffer(&audioBuffer[i].buffer, &contained))) audioBuffersPlaying += contained;
		}
		if ((audioBuffersPlaying == 0 && audioBufferQueueNext >= 2) || audioBuffersPlaying > 2) {
			printf("audioBuffersPlaying %d\n", audioBuffersPlaying);
			for (int i = 0; i < audioBufferQueueNext; i++) {
				audioBufferQueue[i]->enqueued = false;
				audoutAppendAudioOutBuffer(&audioBufferQueue[i]->buffer);
			}
			audioBufferQueueNext = 0;
		}*/
	}

	mutexLock(&emulationLock);
	retro_deinit();
	mutexUnlock(&emulationLock);
}

int main(int argc, char *argv[]) {
#ifdef NXLINK_STDIO
	socketInitializeDefault();
	nxlinkStdio();
#endif

	gfxInitResolutionDefault();
	gfxInitDefault();
	gfxConfigureAutoResolutionDefault(true);

	audioBufferSize = (AUDIO_BUFFER_SAMPLES * 2 * sizeof(u16) + 0xfff) & ~0xfff;
	for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
		AudioOutBuffer *buffer = &audioBuffer[i];
		buffer->next = NULL;
		buffer->buffer = memalign(0x1000, audioBufferSize);
		buffer->buffer_size = audioBufferSize;
		buffer->data_size = 0;
		buffer->data_offset = 0;

		//audioBuffer[i].enqueued = false;
	}

	audoutInitialize();
	audoutStartAudioOut();

	videoTransferBuffer = (u16 *)malloc(256 * 160 * sizeof(u16));
	mutexInit(&videoLock);

	mutexInit(&inputLock);
	mutexInit(&emulationLock);

	Thread mainThread;
	threadCreate(&mainThread, threadFunc, NULL, 0x4000, 0x30, 0);
	threadStart(&mainThread);

	while (appletMainLoop() && running) {
		u32 currentFBWidth, currentFBHeight;
		u8 *currentFB = gfxGetFramebuffer(&currentFBWidth, &currentFBHeight);
		memset(currentFB, 0, sizeof(u32) * currentFBWidth * currentFBHeight);

		hidScanInput();
		u32 keysDown = hidKeysDown(CONTROLLER_P1_AUTO);
		u32 keysHeld = hidKeysHeld(CONTROLLER_P1_AUTO);

		mutexLock(&inputLock);
		inputTransferKeysHeld = keysHeld;
		mutexUnlock(&inputLock);

		bool actionStopEmulation = false;
		bool actionStartEmulation = false;

		UIResult result;
		switch ((result = uiLoop(currentFB, currentFBWidth, currentFBHeight, keysDown))) {
			case resultSelectedFile:
				actionStopEmulation = true;
				actionStartEmulation = true;
				break;
			case resultClose:
				actionStopEmulation = true;
				uiSetState(stateFileselect);
				break;
			case resultExit:
				actionStopEmulation = true;
				running = false;
				break;
			case resultUnpause:
				unpause_emulation();
			case resultLoadState:
			case resultSaveState: {
				mutexLock(&emulationLock);
				char stateFilename[PATH_LENGTH];
				romPathWithExt(stateFilename, "ram");

				u8 *buffer = (u8 *)malloc(serialize_size);

				if (result == resultLoadState) {
					FILE *f = fopen(stateFilename, "rb");
					if (f) {
						if (fread(buffer, 1, serialize_size, f) != serialize_size ||
						    !CPUReadState(buffer, serialize_size))
							uiStatusMsg("Failed to read save state %s", stateFilename);

						fclose(f);
					} else
						printf("Failed to open %s for read", stateFilename);
				} else {
					if (!CPUWriteState(buffer, serialize_size))
						uiStatusMsg("Failed to write save state %s", stateFilename);

					FILE *f = fopen(stateFilename, "wb");
					if (f) {
						if (fwrite(buffer, 1, serialize_size, f) != serialize_size)
							printf("Failed to write to %s", stateFilename);
						fclose(f);
					} else
						printf("Failed to open %s for write", stateFilename);
				}

				free(buffer);
				mutexUnlock(&emulationLock);
			} break;
			case resultIncFrameskip:
				mutexLock(&emulationLock);
				frameSkip = (frameSkip + 1) % (sizeof(frameSkipValues) / sizeof(frameSkipValues[0]));

				uiStatusMsg("Changed frameskip to %s", frameSkipNames[frameSkip]);
				SetFrameskip(frameSkipValues[frameSkip]);

				mutexUnlock(&emulationLock);
				break;
			case resultNone:
			default:
				break;
		}

		if (actionStopEmulation && emulationRunning) {
			mutexLock(&emulationLock);
			retro_unload_game();
			mutexUnlock(&emulationLock);
		}
		if (actionStartEmulation) {
			mutexLock(&emulationLock);

			uiSetState(stateRunning);
			uiGetSelectedFile(currentRomPath);
			retro_load_game();

			SetFrameskip(frameSkipValues[frameSkip]);

			emulationRunning = true;
			emulationPaused = false;

			mutexUnlock(&emulationLock);
		}

		if (emulationRunning && !emulationPaused && keysDown & KEY_X) pause_emulation();

		mutexLock(&videoLock);
		if (emulationRunning && !emulationPaused) {
			unsigned scale = currentFBHeight / 160;
			unsigned offsetX = currentFBWidth / 2 - (scale * 240) / 2;
			unsigned offsetY = currentFBHeight / 2 - (scale * 160) / 2;
			for (int y = 0; y < 160; y++) {
				for (int x = 0; x < 240; x++) {
					int idx0 = x * scale + offsetX + (y * scale + offsetY) * currentFBWidth;
					int idx1 = (x + y * 256);
					u32 val = bgr_555_to_rgb_888_table[videoTransferBuffer[idx1]];
					for (unsigned j = 0; j < scale * currentFBWidth; j += currentFBWidth) {
						for (unsigned i = 0; i < scale; i++) {
							((u32 *)currentFB)[idx0 + i + j] = val;
						}
					}
				}
			}
		}
		mutexUnlock(&videoLock);

		gfxFlushBuffers();
		gfxSwapBuffers();
		gfxWaitForVsync();
	}

	threadWaitForExit(&mainThread);
	threadClose(&mainThread);

	uiDeinit();

	free(videoTransferBuffer);

	for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) free(audioBuffer[i].buffer);

	audoutStopAudioOut();
	audoutExit();

	gfxExit();

#ifdef NXLINK_STDIO
	socketExit();
#endif

	return 0;
}