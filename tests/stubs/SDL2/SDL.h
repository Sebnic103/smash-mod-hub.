// Cabeceras mínimas SOLO para verificar la sintaxis del código de interfaz en un PC sin SDL2.
#pragma once
#include <cstddef>
#include <cstdint>
typedef uint32_t Uint32;
typedef uint8_t Uint8;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Surface { int w, h; };
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
