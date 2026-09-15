/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "video_effects.h"

#include "logging.h"
#include "opentyr.h"
#include "palette.h"
#include "video.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// The effects are drawn by the renderer as overlays on top of the scaled game screen, so they
// work with every scaler and scaling mode.  The CPU work is done at (or below) the game's native
// 320x200 resolution, and nothing at all is done for effects that are off.
//
//   phosphor   Adds the part of the previous frames' afterglow that is brighter than the current
//              frame, so the result is max(current, decayed previous).  The afterglow decays with
//              time rather than per frame, and the last frame is redrawn while it fades out.
//   scanlines  Modulates each game screen row by a brightness profile that is darkest between rows.
//   bloom      Adds a blurred, half-resolution copy of the bright parts of the frame.

const char *const effectLevelNames[EffectLevel_MAX] = {
	"Off",
	"Low",
	"Medium",
	"High",
};

EffectLevel scanlinesLevel = EFFECT_OFF;
EffectLevel bloomLevel = EFFECT_OFF;
EffectLevel phosphorLevel = EFFECT_OFF;

// Darkness between rows, out of 256.
static const unsigned int scanlinesStrengths[EffectLevel_MAX] = { 0, 64, 112, 160 };
// Brightness of the glow, out of 255.
static const Uint8 bloomIntensities[EffectLevel_MAX] = { 0, 96, 160, 224 };
// Brightness that the afterglow retains per PHOSPHOR_DECAY_PERIOD, out of 256.
static const unsigned int phosphorRetentions[EffectLevel_MAX] = { 0, 64, 112, 160 };

enum
{
	GLOW_WIDTH = vga_width / 2,
	GLOW_HEIGHT = vga_height / 2,
	GLOW_THRESHOLD = 192,  // brightest channel value above which pixels start to glow
	GLOW_BLUR_RADIUS = 3,
	GLOW_BLUR_PASSES = 2,
	PHOSPHOR_DECAY_PERIOD = 28,  // ms; about one frame of gameplay
	PHOSPHOR_REFRESH_PERIOD = 33,  // ms; redraw interval while the afterglow fades on a still screen
};

static SDL_Renderer *renderer = NULL;

static SDL_Texture *phosphorTexture = NULL;
static SDL_Texture *scanlinesTexture = NULL;
static SDL_Texture *bloomTexture = NULL;

static int scanlinesTextureHeight = 0;
static EffectLevel scanlinesTextureLevel = EFFECT_OFF;

// The last source frame, which is reused when redrawing.
static Uint8 srcPixels[vga_height][vga_width];
static Uint8 srcPaletteRgb[256][3];

// The frame as seen on screen (including afterglow), in RGB.
static Uint8 frameRgb[vga_height][vga_width][3];
static Uint32 frameTicks = 0;
static bool afterglowVisible = false;

static Uint8 glowRgb[GLOW_HEIGHT][GLOW_WIDTH][3];
static Uint8 glowRgbTemp[GLOW_HEIGHT][GLOW_WIDTH][3];

bool setEffectLevelByName(EffectLevel *const level, const char *const name)
{
	for (int i = 0; i < EffectLevel_MAX; ++i)
	{
		if (strcmp(name, effectLevelNames[i]) == 0)
		{
			*level = i;
			return true;
		}
	}
	return false;
}

void initVideoEffects(SDL_Renderer *const newRenderer)
{
	renderer = newRenderer;
}

static void destroyTexture(SDL_Texture **const texture)
{
	if (*texture != NULL)
	{
		SDL_DestroyTexture(*texture);
		*texture = NULL;
	}
}

void deinitVideoEffects(void)
{
	destroyTexture(&phosphorTexture);
	destroyTexture(&scanlinesTexture);
	destroyTexture(&bloomTexture);

	renderer = NULL;
}

static SDL_Texture *createTexture(int access, int w, int h, SDL_BlendMode blendMode)
{
	SDL_Texture *const texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, access, w, h);
	if (texture == NULL)
	{
		logError("Failed to create effect texture (%dx%d): %s", w, h, SDL_GetError());
		return NULL;
	}

	SDL_SetTextureBlendMode(texture, blendMode);
	return texture;
}

static inline Uint32 packRgb(Uint8 r, Uint8 g, Uint8 b)
{
	return (Uint32)r << 16 | (Uint32)g << 8 | b;
}

static void storeSrcFrame(SDL_Surface *const srcSurface)
{
	for (int y = 0; y < vga_height; ++y)
		memcpy(srcPixels[y], (Uint8 *)srcSurface->pixels + y * srcSurface->pitch, vga_width);

	for (int i = 0; i < 256; ++i)
		SDL_GetRGB(rgb_palette[i], main_window_tex_format, &srcPaletteRgb[i][0], &srcPaletteRgb[i][1], &srcPaletteRgb[i][2]);
}

/** Converts the source frame to RGB, blending in the afterglow and outputting the part of it that is brighter than the source frame. */
static void updateFrameRgb(Uint32 *const phosphorPixels, const int phosphorPitch)
{
	const Uint32 ticks = SDL_GetTicks();
	const Uint32 elapsed = ticks - frameTicks;
	frameTicks = ticks;

	unsigned int retention = 0;
	if (phosphorLevel != EFFECT_OFF)
		retention = (unsigned int)(256 * pow(phosphorRetentions[phosphorLevel] / 256.0, (double)elapsed / PHOSPHOR_DECAY_PERIOD));

	unsigned int afterglowBits = 0;

	for (int y = 0; y < vga_height; ++y)
	{
		Uint32 *const dst = phosphorPixels != NULL ? (Uint32 *)((Uint8 *)phosphorPixels + y * phosphorPitch) : NULL;

		for (int x = 0; x < vga_width; ++x)
		{
			const Uint8 *const color = srcPaletteRgb[srcPixels[y][x]];
			Uint8 *const frame = frameRgb[y][x];
			Uint8 afterglow[3] = { 0, 0, 0 };

			for (int c = 0; c < 3; ++c)
			{
				const Uint8 decayed = frame[c] * retention >> 8;
				frame[c] = MAX(decayed, color[c]);
				afterglow[c] = frame[c] - color[c];
			}

			afterglowBits |= afterglow[0] | afterglow[1] | afterglow[2];

			if (dst != NULL)
				dst[x] = packRgb(afterglow[0], afterglow[1], afterglow[2]);
		}
	}

	afterglowVisible = afterglowBits != 0;
}

/** Blurs each line of RGB samples with a box filter, clamping at the edges. */
static void boxBlur(const Uint8 *const src, Uint8 *const dst, const int length, const int step, const int lineCount, const int lineStep)
{
	assert(length > 2 * GLOW_BLUR_RADIUS + 1);

	const int radius = GLOW_BLUR_RADIUS;
	const int last = length - 1;
	const Uint32 reciprocal = ((1 << 16) + radius) / (2 * radius + 1);  // division by the window size, in 16.16 fixed point

	for (int line = 0; line < lineCount; ++line)
	{
		for (int c = 0; c < 3; ++c)
		{
			const Uint8 *const s = src + line * lineStep + c;
			Uint8 *const d = dst + line * lineStep + c;

			Uint32 sum = (radius + 1) * s[0];
			for (int i = 1; i <= radius; ++i)
				sum += s[i * step];

			int i = 0;
			for (; i < radius; ++i)  // leading edge
			{
				d[i * step] = (sum * reciprocal + 0x8000) >> 16;
				sum += s[(i + radius + 1) * step] - s[0];
			}
			for (; i < last - radius; ++i)  // interior
			{
				d[i * step] = (sum * reciprocal + 0x8000) >> 16;
				sum += s[(i + radius + 1) * step] - s[(i - radius) * step];
			}
			for (; i < length; ++i)  // trailing edge
			{
				d[i * step] = (sum * reciprocal + 0x8000) >> 16;
				sum += s[last * step] - s[(i - radius) * step];
			}
		}
	}
}

static void updateGlow(void)
{
	// Downsample the bright parts of the frame, keeping their hue.
	for (int y = 0; y < GLOW_HEIGHT; ++y)
	{
		for (int x = 0; x < GLOW_WIDTH; ++x)
		{
			const Uint8 *const p0 = frameRgb[2 * y][2 * x], *const p1 = frameRgb[2 * y][2 * x + 1];
			const Uint8 *const p2 = frameRgb[2 * y + 1][2 * x], *const p3 = frameRgb[2 * y + 1][2 * x + 1];

			Uint8 *const glow = glowRgb[y][x];

			int brightest = 0;
			for (int c = 0; c < 3; ++c)
			{
				glow[c] = (p0[c] + p1[c] + p2[c] + p3[c] + 2) / 4;
				brightest = MAX(brightest, glow[c]);
			}

			// Fade the glow in smoothly above the threshold.
			const int excess = MAX(brightest - GLOW_THRESHOLD, 0);
			const int weight = (excess * excess << 8) / ((255 - GLOW_THRESHOLD) * (255 - GLOW_THRESHOLD));

			for (int c = 0; c < 3; ++c)
				glow[c] = glow[c] * weight >> 8;
		}
	}

	for (int pass = 0; pass < GLOW_BLUR_PASSES; ++pass)
	{
		boxBlur(glowRgb[0][0], glowRgbTemp[0][0], GLOW_WIDTH, 3, GLOW_HEIGHT, 3 * GLOW_WIDTH);
		boxBlur(glowRgbTemp[0][0], glowRgb[0][0], GLOW_HEIGHT, 3 * GLOW_WIDTH, GLOW_WIDTH, 3);
	}

	void *pixels;
	int pitch;
	if (SDL_LockTexture(bloomTexture, NULL, &pixels, &pitch) != 0)
		return;

	for (int y = 0; y < GLOW_HEIGHT; ++y)
	{
		Uint32 *const dst = (Uint32 *)((Uint8 *)pixels + y * pitch);

		for (int x = 0; x < GLOW_WIDTH; ++x)
			dst[x] = packRgb(glowRgb[y][x][0], glowRgb[y][x][1], glowRgb[y][x][2]);
	}

	SDL_UnlockTexture(bloomTexture);
}

/** Builds a one-pixel-wide texture that darkens the output rows between game screen rows. */
static bool updateScanlines(const int height)
{
	if (scanlinesTexture != NULL && scanlinesTextureHeight == height && scanlinesTextureLevel == scanlinesLevel)
		return true;

	destroyTexture(&scanlinesTexture);

	Uint32 *const rows = malloc(sizeof(*rows) * height);
	if (rows == NULL)
		return false;

	scanlinesTexture = createTexture(SDL_TEXTUREACCESS_STATIC, 1, height, SDL_BLENDMODE_MOD);
	if (scanlinesTexture == NULL)
	{
		free(rows);
		return false;
	}

	const unsigned int strength = scanlinesStrengths[scanlinesLevel];

	for (int y = 0; y < height; ++y)
	{
		// Distance of the top of this output row from the center of its game screen row, from 0
		// (center) to `height` (edge).  The darkness falls off quadratically towards the center.
		const unsigned int phase = (unsigned int)y * vga_height % height;
		const unsigned int distance = abs(2 * (int)phase - height);
		const unsigned int darkness = strength * distance / height * distance / height;

		const Uint8 brightness = 255 - MIN(darkness, 255);
		rows[y] = packRgb(brightness, brightness, brightness);
	}

	SDL_UpdateTexture(scanlinesTexture, NULL, rows, sizeof(*rows));
	free(rows);

	scanlinesTextureHeight = height;
	scanlinesTextureLevel = scanlinesLevel;
	return true;
}

bool videoEffectsNeedRedraw(void)
{
	return phosphorLevel != EFFECT_OFF && afterglowVisible &&
	       SDL_GetTicks() - frameTicks >= PHOSPHOR_REFRESH_PERIOD;
}

void renderVideoEffects(SDL_Surface *const srcSurface, const SDL_Rect *const dstRect)
{
	assert(renderer != NULL);
	assert(srcSurface == NULL || (srcSurface->w == vga_width && srcSurface->h == vga_height));

	// Textures are created on first use.  If that fails, the effect is turned off.
	if (phosphorLevel != EFFECT_OFF && phosphorTexture == NULL)
	{
		phosphorTexture = createTexture(SDL_TEXTUREACCESS_STREAMING, vga_width, vga_height, SDL_BLENDMODE_ADD);
		if (phosphorTexture == NULL)
			phosphorLevel = EFFECT_OFF;
	}

	if (bloomLevel != EFFECT_OFF && bloomTexture == NULL)
	{
		bloomTexture = createTexture(SDL_TEXTUREACCESS_STREAMING, GLOW_WIDTH, GLOW_HEIGHT, SDL_BLENDMODE_ADD);
		if (bloomTexture == NULL)
			bloomLevel = EFFECT_OFF;
#if SDL_VERSION_ATLEAST(2, 0, 12)
		else
			SDL_SetTextureScaleMode(bloomTexture, SDL_ScaleModeLinear);
#endif
	}

	const bool phosphor = phosphorLevel != EFFECT_OFF;
	const bool bloom = bloomLevel != EFFECT_OFF;
	// Scanlines need at least two output rows per game screen row to be visible.
	const bool scanlines = scanlinesLevel != EFFECT_OFF && dstRect->h >= 2 * vga_height;

	if ((phosphor || bloom) && srcSurface != NULL)
		storeSrcFrame(srcSurface);

	if (phosphor)
	{
		void *pixels;
		int pitch;
		if (SDL_LockTexture(phosphorTexture, NULL, &pixels, &pitch) == 0)
		{
			updateFrameRgb(pixels, pitch);
			SDL_UnlockTexture(phosphorTexture);

			SDL_RenderCopy(renderer, phosphorTexture, NULL, dstRect);
		}
	}
	else if (bloom)
	{
		updateFrameRgb(NULL, 0);
	}

	if (scanlines && updateScanlines(dstRect->h))
		SDL_RenderCopy(renderer, scanlinesTexture, NULL, dstRect);

	if (bloom)
	{
		updateGlow();

		const Uint8 intensity = bloomIntensities[bloomLevel];
		SDL_SetTextureColorMod(bloomTexture, intensity, intensity, intensity);
		SDL_RenderCopy(renderer, bloomTexture, NULL, dstRect);
	}
}
