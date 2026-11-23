// Cabeceras mínimas SOLO para verificar la sintaxis del código de interfaz en un PC sin SDL2.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
typedef uint32_t Uint32;
typedef uint8_t Uint8;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Surface { int w, h; void* pixels; int pitch; };
struct SDL_Color { Uint8 r, g, b, a; };
struct SDL_Rect { int x, y, w, h; };
struct SDL_RWops;
enum SDL_BlendMode { SDL_BLENDMODE_NONE = 0, SDL_BLENDMODE_BLEND = 1 };
enum { SDL_PIXELFORMAT_RGBA32 = 1 };
SDL_RWops* SDL_RWFromMem(void*, int);
int SDL_SetRenderDrawBlendMode(SDL_Renderer*, SDL_BlendMode);
int SDL_SetRenderDrawColor(SDL_Renderer*, Uint8, Uint8, Uint8, Uint8);
int SDL_RenderFillRect(SDL_Renderer*, const SDL_Rect*);
int SDL_RenderCopy(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*);
int SDL_RenderClear(SDL_Renderer*);
enum SDL_bool { SDL_FALSE = 0, SDL_TRUE = 1 };
int SDL_RenderSetClipRect(SDL_Renderer*, const SDL_Rect*);
void SDL_RenderGetClipRect(SDL_Renderer*, SDL_Rect*);
SDL_bool SDL_RenderIsClipEnabled(SDL_Renderer*);
int SDL_SetTextureAlphaMod(SDL_Texture*, Uint8);
int SDL_QueryTexture(SDL_Texture*, Uint32*, int*, int*, int*);
SDL_Texture* SDL_CreateTextureFromSurface(SDL_Renderer*, SDL_Surface*);
void SDL_FreeSurface(SDL_Surface*);
void SDL_DestroyTexture(SDL_Texture*);
SDL_Surface* SDL_ConvertSurfaceFormat(SDL_Surface*, Uint32, Uint32);
SDL_Surface* SDL_CreateRGBSurfaceWithFormat(Uint32, int, int, int, Uint32);
int SDL_SetSurfaceBlendMode(SDL_Surface*, SDL_BlendMode);
int SDL_BlitScaled(SDL_Surface*, const SDL_Rect*, SDL_Surface*, SDL_Rect*);

// --- audio (solo para comprobar la sintaxis de audio.cpp)
typedef uint32_t SDL_AudioDeviceID;
typedef uint16_t SDL_AudioFormat;
struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels, silence;
    uint16_t samples;
    uint32_t size;
    void (*callback)(void*, Uint8*, int);
    void* userdata;
};
#define SDL_INIT_AUDIO 0x10u
#define AUDIO_S16SYS 0x8010
#define SDL_zero(x) memset(&(x), 0, sizeof((x)))
int SDL_InitSubSystem(uint32_t);
void SDL_QuitSubSystem(uint32_t);
const char* SDL_GetError();
SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int);
void SDL_CloseAudioDevice(SDL_AudioDeviceID);
void SDL_PauseAudioDevice(SDL_AudioDeviceID, int);
void SDL_LockAudioDevice(SDL_AudioDeviceID);
void SDL_UnlockAudioDevice(SDL_AudioDeviceID);

int SDL_SetTextureBlendMode(SDL_Texture*, SDL_BlendMode);
int SDL_RenderDrawLine(SDL_Renderer*, int, int, int, int);
