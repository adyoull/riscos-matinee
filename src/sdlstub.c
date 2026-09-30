/*
 * sdlstub.c - the few SDL2 calls reelcore makes, without SDL.
 *
 * reelcore plays sound through SharedSoundBuffer/StreamManager on RISC OS,
 * and only opens SDL's audio when REELCORE_AUDIO=sdl or SharedSoundBuffer
 * is missing. Linking riscos-mesa's libSDL2.a for that would bring its
 * video and OpenGL parts (and OSMesa) too, so PlexRO answers those calls
 * here: SDL's audio can't be opened ("no sound", with the reason), and
 * SDL_MixAudioFormat mixes at a volume as SDL's does.
 * Part of riscos-plex. GPL v2 or later.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char no_sdl[] = "SharedSoundBuffer and StreamManager are needed for sound";

int SDL_InitSubSystem(uint32_t flags) { (void)flags; return -1; }
uint32_t SDL_WasInit(uint32_t flags) { (void)flags; return 0; }
const char *SDL_GetError(void) { return no_sdl; }
const char *SDL_GetCurrentAudioDriver(void) { return NULL; }
char *SDL_getenv(const char *name) { return getenv(name); }
int SDL_setenv(const char *name, const char *value, int overwrite)
{
    (void)name; (void)value; (void)overwrite;
    return 0;
}

uint32_t SDL_OpenAudioDevice(const char *device, int iscapture, const void *want, void *have, int changes)
{
    (void)device; (void)iscapture; (void)want; (void)have; (void)changes;
    return 0;
}
void SDL_CloseAudioDevice(uint32_t dev) { (void)dev; }
void SDL_PauseAudioDevice(uint32_t dev, int pause) { (void)dev; (void)pause; }
int SDL_QueueAudio(uint32_t dev, const void *data, uint32_t len) { (void)dev; (void)data; (void)len; return -1; }
uint32_t SDL_GetQueuedAudioSize(uint32_t dev) { (void)dev; return 0; }
void SDL_ClearQueuedAudio(uint32_t dev) { (void)dev; }

/* 16-bit samples (the only format reelcore mixes), added into dst at
   volume/128, clipped, as SDL does */
void SDL_MixAudioFormat(uint8_t *dst, const uint8_t *src, uint16_t format, uint32_t len, int volume)
{
    int16_t *d = (int16_t *)dst;
    const int16_t *s = (const int16_t *)src;
    (void)format;
    if (volume <= 0)
        return;
    if (volume > 128)
        volume = 128;
    for (uint32_t i = 0; i < len / 2; i++) {
        int v = d[i] + s[i] * volume / 128;
        d[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}
