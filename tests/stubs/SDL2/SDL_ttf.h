#pragma once
#include "SDL.h"
struct TTF_Font;
TTF_Font* TTF_OpenFontRW(SDL_RWops*, int, int);
void TTF_CloseFont(TTF_Font*);
SDL_Surface* TTF_RenderUTF8_Blended(TTF_Font*, const char*, SDL_Color);
SDL_Surface* TTF_RenderUTF8_Blended_Wrapped(TTF_Font*, const char*, SDL_Color, Uint32);
int TTF_SizeUTF8(TTF_Font*, const char*, int*, int*);
int TTF_FontLineSkip(const TTF_Font*);
