#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <X11/Xft/Xft.h>
#include <hb.h>
#include <hb-ft.h>

#include "st.h"

void hbtransformsegment(XftFont *xfont, const Glyph *string, hb_codepoint_t *codepoints, int start, int length);
hb_font_t *hbfindfont(XftFont *match);

typedef struct {
	XftFont *match;
	hb_font_t *font;
} HbFontMatch;

static int hbfontslen = 0;
static HbFontMatch *hbfontcache = NULL;

void
hbunloadfonts()
{
	for (int i = 0; i < hbfontslen; i++) {
		hb_font_destroy(hbfontcache[i].font);
		XftUnlockFace(hbfontcache[i].match);
	}

	if (hbfontcache != NULL) {
		free(hbfontcache);
		hbfontcache = NULL;
	}
	hbfontslen = 0;
}

hb_font_t *
hbfindfont(XftFont *match)
{
	for (int i = 0; i < hbfontslen; i++) {
		if (hbfontcache[i].match == match)
			return hbfontcache[i].font;
	}

	/* Font not found in cache, caching it now. */
	hbfontcache = realloc(hbfontcache, sizeof(HbFontMatch) * (hbfontslen + 1));
	FT_Face face = XftLockFace(match);
	hb_font_t *font = hb_ft_font_create(face, NULL);
	if (font == NULL)
		die("Failed to load Harfbuzz font.");

	hbfontcache[hbfontslen].match = match;
	hbfontcache[hbfontslen].font = font;
	hbfontslen += 1;

	return font;
}

void
hbtransform(XftGlyphFontSpec *specs, const Glyph *glyphs, size_t len, int x, int y)
{
	int start = 0, length = 1, gstart = 0;
	hb_codepoint_t *codepoints = calloc(len, sizeof(hb_codepoint_t));

	for (int idx = 1, specidx = 1; idx < len; idx++) {
		if (glyphs[idx].mode & ATTR_WDUMMY) {
			length += 1;
			continue;
		}

		if (specs[specidx].font != specs[start].font || ATTRCMP(glyphs[gstart], glyphs[idx]) || selected(x + idx, y) != selected(x + gstart, y)) {
			hbtransformsegment(specs[start].font, glyphs, codepoints, gstart, length);

			/* Reset the sequence. */
			length = 1;
			start = specidx;
			gstart = idx;
		} else {
			length += 1;
		}

		specidx++;
	}

	/* EOL. */
	hbtransformsegment(specs[start].font, glyphs, codepoints, gstart, length);

	/* Apply the transformation to glyph specs. */
	for (int i = 0, specidx = 0; i < len; i++) {
		if (glyphs[i].mode & ATTR_WDUMMY)
			continue;
		if (glyphs[i].mode & ATTR_BOXDRAW) {
			specidx++;
			continue;
		}

		if (codepoints[i] != specs[specidx].glyph)
			((Glyph *)glyphs)[i].mode |= ATTR_LIGA;

		specs[specidx++].glyph = codepoints[i];
	}

	free(codepoints);
}

void
hbtransformsegment(XftFont *xfont, const Glyph *string, hb_codepoint_t *codepoints, int start, int length)
{
	hb_font_t *font = hbfindfont(xfont);
	if (font == NULL)
		return;

	Rune rune;
	ushort mode = USHRT_MAX;
	hb_buffer_t *buffer = hb_buffer_create();
	hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);

	/* Fill buffer with codepoints. */
	for (int i = start; i < (start+length); i++) {
		rune = string[i].u;
		mode = string[i].mode;
		if (mode & ATTR_WDUMMY)
			rune = 0x0020;
		hb_buffer_add_codepoints(buffer, &rune, 1, 0, 1);
	}

	/* Shape the segment. */
	hb_shape(font, buffer, NULL, 0);

	/* Get new glyph info. */
	hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, NULL);

	/* Write new codepoints. */
	for (int i = 0; i < length; i++) {
		hb_codepoint_t gid = info[i].codepoint;
		codepoints[start+i] = gid;
	}

	/* Cleanup. */
	hb_buffer_destroy(buffer);
}
