/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// snd_mem.c: sound caching

#include "common.h"
#include "console.h"
#include "quakedef.h"
#include "sound.h"
#include "sys.h"

typedef struct {
    int rate;
    int width;
    int channels;
    int loopstart;
    int samples;
    int dataofs;		// chunk starts this many bytes from file start
} wavinfo_t;

static wavinfo_t *GetWavinfo(const char *name, const byte *wav, int wavlength);

/*
================
ResampleSfx
================
*/
static void
ResampleSfx(sfx_t *sfx, int inrate, int inwidth, const byte *data)
{
    int outcount;
    int srcsample;
    float stepscale;
    int i;
    int sample, samplefrac, fracstep;
    sfxcache_t *sc;

    sc = Cache_Check(&sfx->cache);
    if (!sc)
	return;

    stepscale = (float)inrate / shm->speed;	// this is usually 0.5, 1, or 2

    outcount = sc->length / stepscale;
    sc->length = outcount;
    if (sc->loopstart != -1)
	sc->loopstart = sc->loopstart / stepscale;

    sc->speed = shm->speed;
    if (loadas8bit.value)
	sc->width = 1;
    else
	sc->width = inwidth;
    sc->stereo = 0;

// resample / decimate to the current source rate

    if (stepscale == 1 && inwidth == 1 && sc->width == 1) {
// fast special case
	for (i = 0; i < outcount; i++)
	    ((signed char *)sc->data)[i]
		= (int)((unsigned char)(data[i]) - 128);
    } else {
// general case
	samplefrac = 0;
	fracstep = stepscale * 256;
	for (i = 0; i < outcount; i++) {
	    srcsample = samplefrac >> 8;
	    samplefrac += fracstep;
	    if (inwidth == 2)
		sample = LittleShort(((const short *)data)[srcsample]);
	    else
		sample = (int)((unsigned char)(data[srcsample]) - 128) << 8;
	    if (sc->width == 2)
		((short *)sc->data)[i] = sample;
	    else
		((signed char *)sc->data)[i] = sample >> 8;
	}
    }
}

//=============================================================================

/*
==============
S_LoadSound
==============
*/
sfxcache_t *
S_LoadSound(sfx_t *s)
{
    byte *data;
    size_t datasize;
    wavinfo_t *info;
    int len;
    float stepscale;
    sfxcache_t *sc;
    char namebuffer[256];
    byte stackbuf[1024];	// avoid dirtying the cache heap

    sc = Cache_Check(&s->cache);
    if (sc)
	return sc;

    /* load it in */
    qsnprintf(namebuffer, sizeof(namebuffer), "sound/%s", s->name);
    data = COM_LoadStackFile(namebuffer, stackbuf, sizeof(stackbuf), &datasize);
    if (!data) {
	Con_Printf("Couldn't load %s\n", namebuffer);
	return NULL;
    }

    info = GetWavinfo(s->name, data, datasize);
    if (info->channels != 1) {
	Con_Printf("%s is a stereo sample\n", s->name);
	return NULL;
    }

    stepscale = (float)info->rate / shm->speed;
    len = info->samples / stepscale;

    len = len * info->width * info->channels;

    sc = Cache_Alloc(&s->cache, len + sizeof(sfxcache_t), s->name);
    if (!sc)
	return NULL;

    sc->length = info->samples;
    sc->loopstart = info->loopstart;
    sc->speed = info->rate;
    sc->width = info->width;
    sc->stereo = info->channels;

    ResampleSfx(s, sc->speed, sc->width, data + info->dataofs);

    return sc;
}



/*
===============================================================================

WAV loading

===============================================================================
*/


static const byte *data_p;
static const byte *iff_end;
static const byte *last_chunk;
static const byte *iff_data;
static int iff_chunk_len;


static short
GetLittleShort(void)
{
    short val = 0;

    val = *data_p;
    val = val + (*(data_p + 1) << 8);
    data_p += 2;
    return val;
}

static int
GetLittleLong(void)
{
    int val = 0;

    val = *data_p;
    val = val + (*(data_p + 1) << 8);
    val = val + (*(data_p + 2) << 16);
    val = val + (*(data_p + 3) << 24);
    data_p += 4;
    return val;
}

static void
FindNextChunk(const char *name, const char *filename)
{
    while (1) {
	/* Need at least 8 bytes for a chunk */
	if (last_chunk + 8 >= iff_end) {
	    data_p = NULL;
	    return;
	}

	data_p = last_chunk + 4;
	iff_chunk_len = GetLittleLong();
	if (iff_chunk_len < 0 || iff_chunk_len > iff_end - data_p) {
	    Con_DPrintf("Bad \"%s\" chunk length (%d) in wav file %s\n",
			name, iff_chunk_len, filename);
	    data_p = NULL;
	    return;
	}
	last_chunk = data_p + ((iff_chunk_len + 1) & ~1);
	data_p -= 8;
	if (!strncmp((const char *)data_p, name, 4))
	    return;
    }
}

static void
FindChunk(const char *name, const char *filename)
{
    last_chunk = iff_data;
    FindNextChunk(name, filename);
}

#if 0
static void
DumpChunks(void)
{
    char str[5];

    str[4] = 0;
    data_p = iff_data;
    do {
	memcpy(str, data_p, 4);
	data_p += 4;
	iff_chunk_len = GetLittleLong();
	Con_Printf("0x%x : %s (%d)\n", (int)(data_p - 4), str, iff_chunk_len);
	data_p += (iff_chunk_len + 1) & ~1;
    } while (data_p < iff_end);
}
#endif

/*
============
GetWavinfo
============
*/
static wavinfo_t *
GetWavinfo(const char *name, const byte *wav, int wavlength)
{
    static wavinfo_t info;
    int i;
    int format;
    int samples;

    memset(&info, 0, sizeof(info));

    if (!wav)
	return &info;

    iff_data = wav;
    iff_end = wav + wavlength;

// find "RIFF" chunk
    FindChunk("RIFF", name);
    if (!(data_p && !strncmp((char *)data_p + 8, "WAVE", 4))) {
	Con_Printf("Missing RIFF/WAVE chunks\n");
	return &info;
    }
// get "fmt " chunk
    iff_data = data_p + 12;
// DumpChunks ();

    FindChunk("fmt ", name);
    if (!data_p) {
	Con_Printf("Missing fmt chunk\n");
	return &info;
    }
    data_p += 8;
    format = GetLittleShort();
    if (format != 1) {
	Con_Printf("Microsoft PCM format only\n");
	return &info;
    }

    info.channels = GetLittleShort();
    info.rate = GetLittleLong();
    data_p += 4 + 2;
    info.width = GetLittleShort() / 8;

// get cue chunk
    FindChunk("cue ", name);
    if (data_p) {
	data_p += 32;
	info.loopstart = GetLittleLong();

	// if the next chunk is a LIST chunk, look for a cue length marker
	FindNextChunk("LIST", name);
	if (data_p) {
	    /* this is not a proper parse, but it works with cooledit... */
	    if (!strncmp((char *)data_p + 28, "mark", 4)) {
		data_p += 24;
		i = GetLittleLong();	// samples in loop
		info.samples = info.loopstart + i;
	    }
	}
    } else
	info.loopstart = -1;

// find data chunk
    FindChunk("data", name);
    if (!data_p) {
	Con_Printf("Missing data chunk\n");
	return &info;
    }

    data_p += 4;
    samples = GetLittleLong() / info.width;

    if (info.samples) {
	if (samples < info.samples)
	    Sys_Error("Sound %s has a bad loop length", name);
    } else
	info.samples = samples;

    info.dataofs = data_p - wav;

    return &info;
}
