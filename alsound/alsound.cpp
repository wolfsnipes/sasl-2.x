#include "alsound.h"
#include "timer.h"

#include <string>
#include <vector>
#include <queue>
#include <map>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <fstream>

SASL ALChecker::sasl = NULL;

namespace sasl {

	// changes context to specified and restores old context after destruction
	class ContextChanger
	{
	private:
		// saved old context
		ALCcontext* oldContext;
	public:
		// save old context and change to our context
		ContextChanger(ALCcontext *context) {
			oldContext = alcGetCurrentContext();
			alcCheck(alcMakeContextCurrent(context), alcGetContextsDevice(context));
		};

		// restore saved context
		~ContextChanger() {
			alcCheck(alcMakeContextCurrent(oldContext), alcGetContextsDevice(oldContext));
		}
	};

};

using namespace sasl;
using namespace timer;

// reference counted buffer
struct Buffer
{
	ALuint id;
	int usage;
};


// source associated with buffer
struct Source
{
	ALuint id;
	Buffer *buffer;
	int scene;
	float gain;
	Timer* timer;
	double duration;
};

// sound parameters
struct SaslAlSound
{
	// xap sound api
	SaslSoundCallbacks callbacks;

	// OpenAL devices
	std::vector<ALCdevice*> devices;

	ALCcontext* getAppropriateContext() {
		int inContext;
		if (!freeIDs.empty()) {
			return getContextBySample(freeIDs.front());
		} else {
			inContext = curSourceNumber / maxSourcesPerContext;
			if (inContext > contexts.size() - 1) {
#if APL
				devices.push_back(alcOpenDevice("Generic Software"));
				alcCheck(contexts.push_back(alcCreateContext(devices.back(), NULL)), devices.back());
#else
				alcCheck(contexts.push_back(alcCreateContext(devices[0], NULL)), devices.back());
#endif
			}
			return contexts.back();
		}
	}

	ALCcontext* getContextBySample(int inSampleID) {
		return contexts[std::size_t((inSampleID - 1)  / maxSourcesPerContext)];
	}

	// OpenAL context
	std::vector<ALCcontext*> contexts;

	// loaded sound files
	std::map<std::string, Buffer> buffers;

	// sources in use
	std::map<int, Source> sources;

	// free IDs after sample unloads
	std::queue<int> freeIDs;

	// maximum source number
	int curSourceNumber;

	// max sources per context
	int maxSourcesPerContext;

	// current scene
	int scene;

	// reference to sasl
	SASL sasl;
};



/**************************************************************************************************************
* WAVE FILE LOADING

**************************************************************************************************************/

// You can just use alutCreateBufferFromFile to load a wave file, but there seems to be a lot of problems with 
// alut not beign available, being deprecated, etc.  So...here's a stupid routine to load a wave file.  I have
// tested this only on x86 machines, so if you find a bug on PPC please let me know.

// Macros to swap endian-values.

#define SWAP_32(value)                 \
	(((((uint16_t)value) << 8) & 0xFF00) | \
	((((uint16_t)value) >> 8) & 0x00FF))

#define SWAP_16(value)                     \
	(((((uint32_t)value) << 24) & 0xFF000000) | \
	((((uint32_t)value) << 8) & 0x00FF0000) | \
	((((uint32_t)value) >> 8) & 0x0000FF00) | \
	((((uint32_t)value) >> 24) & 0x000000FF))

// Wave files are RIFF files, which are "chunky" - each section has an ID and a length.  This lets us skip
// things we can't understand to find the parts we want.  This header is common to all RIFF chunks.
struct chunk_header {
	int32_t	id;
	int32_t	size;
};

// WAVE file format info.  We pass this through to OpenAL so we can support mono/stereo, 8/16/bit, etc.
struct format_info {
	int16_t format;				// PCM = 1, not sure what other values are legal.
	int16_t num_channels;
	int32_t sample_rate;
	int32_t byte_rate;
	int16_t block_align;
	int16_t bits_per_sample;
};

// This utility returns the start of data for a chunk given a range of bytes it might be within.  Pass 1 for
// swapped if the machine is not the same endian as the file.
static char* find_chunk(char *file_begin, char *file_end,
	int desired_id, int swapped)
{
	while (file_begin < file_end) {
		chunk_header *h = (chunk_header*)file_begin;
		if (h->id == desired_id && !swapped)
			return file_begin + sizeof(chunk_header);
		if (h->id == SWAP_32(desired_id) && swapped)
			return file_begin + sizeof(chunk_header);
		int32_t chunk_size = swapped ? SWAP_32(h->size) : h->size;
		char *next = file_begin + chunk_size + sizeof(chunk_header);
		if (next > file_end || next <= file_begin)
			return NULL;
		file_begin = next;
	}
	return NULL;
}

// Given a chunk, find its end by going back to the header.
static char* chunk_end(char *chunk_start, int swapped)
{
	chunk_header *h = (chunk_header*)(chunk_start - sizeof(chunk_header));
	return chunk_start + (swapped ? SWAP_32(h->size) : h->size);
}

#define FAIL(X) { sasl_log_error(sasl, X); free(mem); return 0; }

#define RIFF_ID 0x46464952			// 'RIFF'
#define FMT_ID  0x20746D66			// 'FMT '
#define DATA_ID 0x61746164			// 'DATA'


static ALuint load_wave(SASL sasl, const char *file_name, bool need_reversed)
{
	// First: we open the file and copy it into a single large memory buffer for processing.

	FILE *fi = fopen(file_name, "rb");
	if (!fi) {
		sasl_log_error(sasl, "WAVE file load failed - could not open.");
		return 0;
	}
	fseek(fi, 0, SEEK_END);
	std::size_t file_size = ftell(fi);
	fseek(fi, 0, SEEK_SET);
	char *mem = (char*)malloc(file_size);
	if (!mem) {
		sasl_log_error(sasl, "WAVE file load failed - could not allocate memory.");
		fclose(fi);
		return 0;
	}
	if (fread(mem, 1, file_size, fi) != file_size) {
		sasl_log_error(sasl, "WAVE file load failed - could not read file.");
		free(mem);
		fclose(fi);
		return 0;
	}
	fclose(fi);
	char *mem_end = mem + file_size;

	// Second: find the RIFF chunk.  Note that by searching for RIFF both normal
	// and reversed, we can automatically determine the endian swap situation for
	// this file regardless of what machine we are on.

	int32_t swapped = 0;
	char *riff = find_chunk(mem, mem_end, RIFF_ID, 0);
	if (!riff) {
		riff = find_chunk(mem, mem_end, RIFF_ID, 1);
		if (riff)
			swapped = 1;
		else
			FAIL("Could not find RIFF chunk in wave file.\n")
	}

	// The wave chunk isn't really a chunk at all. :-(  It's just a "WAVE" tag 
	// followed by more chunks.  This strikes me as totally inconsistent, but
	// anyway, confirm the WAVE ID and move on.

	if (riff[0] != 'W' ||
		riff[1] != 'A' ||
		riff[2] != 'V' ||
		riff[3] != 'E')
		FAIL("Could not find WAVE signature in wave file.\n")

		char *format = find_chunk(riff + 4, chunk_end(riff, swapped), FMT_ID, swapped);
	if (!format)
		FAIL("Could not find FMT  chunk in wave file.\n")

		// Find the format chunk, and swap the values if needed.  This gives us our real format.

		format_info *fmt = (format_info *)format;
	if (swapped) {
		fmt->format = SWAP_16(fmt->format);
		fmt->num_channels = SWAP_16(fmt->num_channels);
		fmt->sample_rate = SWAP_32(fmt->sample_rate);
		fmt->byte_rate = SWAP_32(fmt->byte_rate);
		fmt->block_align = SWAP_16(fmt->block_align);
		fmt->bits_per_sample = SWAP_16(fmt->bits_per_sample);
	}

	// Reject things we don't understand...expand this code to support weirder audio formats.

	if (fmt->format != 1) FAIL("Wave file is not PCM format data.\n")
	if (fmt->num_channels != 1 && fmt->num_channels != 2) FAIL("Must have mono or stereo sound.\n")
	if (fmt->bits_per_sample != 8 && fmt->bits_per_sample != 16) FAIL("Must have 8 or 16 bit sounds.\n")
		char *data = find_chunk(riff + 4, chunk_end(riff, swapped), DATA_ID, swapped);
	if (!data)
		FAIL("I could not find the DATA chunk.\n")

		int32_t sample_size = fmt->num_channels * fmt->bits_per_sample / 8;
	int32_t data_bytes = chunk_end(data, swapped) - data;
	int32_t data_samples = data_bytes / sample_size;

	// If the file is swapped and we have 16-bit audio, we need to endian-swap the audio too or we'll 
	// get something that sounds just astoundingly bad!

	if (fmt->bits_per_sample == 16 && swapped) {
		int16_t *ptr = (short*)data;
		int32_t words = data_samples * fmt->num_channels;
		while (words--) {
			*ptr = SWAP_16(*ptr);
			++ptr;
		}
	}

	// Check if there needed a reversed data

	std::vector<char> reversed_data;

	if (need_reversed) {
		for (char* p = data; p != data + data_bytes - 1; p++) {
			reversed_data.push_back(*p);
		}

		std::reverse(reversed_data.begin(), reversed_data.end());
	}

	// Finally, the OpenAL crud.  Build a new OpenAL buffer and send the data to OpenAL, passing in
	// OpenAL format enums based on the format chunk.

	ALuint buf_id = 0;
	alGenBuffers(1, &buf_id);
	if (!buf_id) FAIL("Could not generate buffer id.\n");

	alBufferData(buf_id, fmt->bits_per_sample == 16 ?
		(fmt->num_channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16) :
		(fmt->num_channels == 2 ? AL_FORMAT_STEREO8 : AL_FORMAT_MONO8),
		need_reversed ? &reversed_data[0] : data,
		data_bytes, fmt->sample_rate);
	free(mem);
	return buf_id;
}

// load sound from disk or reuses already loaded 
static int loadSoundIntoAL(struct SaslSoundCallbacks *callbacks, const char *fileName, bool need_reversed, bool need_timer)
{
	SaslAlSound *sound = (SaslAlSound*)callbacks;

	if ((!sound) || (!fileName) || (!sound->devices[0])) {
		return 0;
	}

	ALCcontext* context = sound->getAppropriateContext();

	ContextChanger changer(context);

	ALuint bufId = 0;
	std::map<std::string, Buffer>::iterator i = sound->buffers.find(fileName);
	if (i == sound->buffers.end()) {
		bufId = load_wave(sound->sasl, fileName, need_reversed);
		if (!bufId) {
			sasl_log_error(sound->sasl, "Can't load sound '%s'", fileName);
			return 0;
		}
	} else {
		Buffer &b = (*i).second;
		bufId = b.id;
	}

	ALuint source;
	alGenSources(1, &source);
	if (AL_NO_ERROR != alGetError()) {
		sasl_log_error(sound->sasl, "Can't create sound source");
		return 0;
	}

	ALfloat zero[] = { 0, 0, 0 };

	alSourcei(source, AL_BUFFER, bufId);
	alSourcef(source, AL_PITCH, 1.0f);
	alSourcef(source, AL_GAIN, 1.0f);
	alSourcefv(source, AL_POSITION, zero);
	alSourcefv(source, AL_VELOCITY, zero);
	alSourcei(source, AL_LOOPING, 0);
	alSourcei(source, AL_SOURCE_RELATIVE, 1);

	if (AL_NO_ERROR != alGetError()) {
		sasl_log_error(sound->sasl, "Can't set sound source properties");
		alDeleteSources(1, &source);
		return 0;
	}

	Source src;
	src.id = source;
	src.scene = SOUND_EVERYWHERE;
	src.gain = 1.0f;
	if (need_timer) {
		ALint bufSizeBytes, bufChannels, bufbits, bufFrequency;

		alGetBufferi(bufId, AL_SIZE, &bufSizeBytes);
		alGetBufferi(bufId, AL_CHANNELS, &bufChannels);
		alGetBufferi(bufId, AL_BITS, &bufbits);
		alGetBufferi(bufId, AL_FREQUENCY, &bufFrequency);

		src.duration = (double)(bufSizeBytes * 8 / (bufChannels * bufbits)) / (double)bufFrequency;
		src.timer = new Timer();
	} else {
		src.duration = 0.0;
		src.timer = NULL;
	}

	if (i == sound->buffers.end()) {
		Buffer buffer;
		buffer.id = bufId;
		buffer.usage = 1;
		sound->buffers[fileName] = buffer;
		src.buffer = &sound->buffers[fileName];
	} else {
		Buffer &b = (*i).second;
		b.usage++;
		src.buffer = &b;
	}

	if (sound->freeIDs.empty()) {
		sound->curSourceNumber++;
		sound->sources[sound->curSourceNumber] = src;

		return sound->curSourceNumber;
	} else {
		int sourceSoundID = sound->freeIDs.front();
		sound->freeIDs.pop();
		sound->sources[sourceSoundID] = src;
		return sourceSoundID;
	}
}

static int loadSound(struct SaslSoundCallbacks *callbacks, const char *fileName, bool needTimer)
{
	return loadSoundIntoAL(callbacks, fileName, false, needTimer);
}

static int loadSoundReversed(struct SaslSoundCallbacks *callbacks, const char *fileName, bool needTimer)
{
	return loadSoundIntoAL(callbacks, fileName, true, needTimer);
}

// remove buffer with specified id
static void deleteBuffer(SaslAlSound *sound, ALuint id)
{
	for (std::map<std::string, Buffer>::iterator i = sound->buffers.begin();
		i != sound->buffers.end(); ++i)
	{
		if ((*i).second.id == id) {
			alCheck(alDeleteBuffers(1, &(*i).second.id));
			sound->buffers.erase(i);
			return;
		}
	}
}


// unload sound
static void unloadSound(struct SaslSoundCallbacks *s, int sampleId)
{
	SaslAlSound *sound = (SaslAlSound*)s;

	if ((!sound) || (!sound->devices[0]) || (!sampleId) || sound->contexts.empty()) {
		return;
	}

	std::map<int, Source>::iterator i = sound->sources.find(sampleId);
	if (i == sound->sources.end())
		return;
	Source &source = (*i).second;

	ContextChanger changer(sound->getContextBySample(sampleId));

	alCheck(alDeleteSources(1, &source.id));

	source.buffer->usage--;
	if (0 >= source.buffer->usage)
		deleteBuffer(sound, source.buffer->id);

	if (source.timer) {
		delete source.timer;
		source.timer = NULL;
	}
	sound->sources.erase(i);

	sound->freeIDs.push(sampleId);
}


// returns source ID or -1 if error
static ALuint getSource(struct SaslSoundCallbacks *s, int sampleId)
{
	SaslAlSound *sound = (SaslAlSound*)s;

	if ((!sound) || (!sound->devices[0]) || (sound->contexts.empty()) || (!sampleId)) {
		return (ALuint)-1;
	}

	std::map<int, Source>::const_iterator i = sound->sources.find(sampleId);
	if (i == sound->sources.end()) {
		return (ALuint)-1;
	}

	return (*i).second.id;
}

// play sound source
static void playSound(struct SaslSoundCallbacks *s, int sampleId, int loop)
{
	alGetError();
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));

	ALfloat zero[] = { 0, 0, 0 };
	ALfloat orient[] = { 0, 0, -1, 0, 1, 0 };

	alSourcei(source, AL_LOOPING, loop);
	alListenerfv(AL_POSITION, zero);
	alListenerfv(AL_VELOCITY, zero);
	alListenerfv(AL_ORIENTATION, orient);

	ALuint err = alGetError();
	if (AL_NO_ERROR != err)
		sasl_log_error(sound->sasl, "Setup error %i", err);
	alSourcePlay(source);
	if (sound->sources[sampleId].timer) {
		sound->sources[sampleId].timer->start();
	}

	err = alGetError();
	if (AL_NO_ERROR != err)
		sasl_log_error(sound->sasl, "Play error %i", err);
}


// stop sample playing
static void stopSound(struct SaslSoundCallbacks *s, int sampleId)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	alCheck(alSourceStop(source));
	if (sound->sources[sampleId].timer) {
		sound->sources[sampleId].timer->stop();
	}
}

// Get time that remaining to play for current sample
static void getSamplePlayingLeft(struct SaslSoundCallbacks *s, int sampleId, double* left)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		*left = 0.0;
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	ALenum state;
	ALint loop;
	alCheck(alGetSourcei(source, AL_SOURCE_STATE, &state));
	alCheck(alGetSourcei(source, AL_LOOPING, &loop));

	if (sound->sources[sampleId].timer) {
		if (state == AL_PLAYING) {
			double elapsedPlaying = sound->sources[sampleId].timer->getElapsedSec();
			if (!loop) {
				if (elapsedPlaying >= sound->sources[sampleId].duration) {
					sound->sources[sampleId].timer->stop();
					*left = 0.0;
				} else {
					*left = sound->sources[sampleId].duration - elapsedPlaying;
				}
			} else {
				*left = fmod(elapsedPlaying, sound->sources[sampleId].duration);
			}
		} else {
			*left = 0.0;
			sound->sources[sampleId].timer->stop();
		}
	} else {
		if (state == AL_PLAYING) {
			*left = 1.0;
		} else {
			*left = 0.0;
		}
	}

}

// set gain of sound
static void setGain(struct SaslSoundCallbacks *s, int sampleId, int gain)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint srcId = getSource(s, sampleId);
	if ((ALuint)-1 == srcId) {
		return;
	}

	Source &source = sound->sources[sampleId];
	source.gain = (float)gain / 1000.0;

	if (sound->scene & source.scene) {
		ContextChanger changer(sound->getContextBySample(sampleId));
		alCheck(alSourcef(srcId, AL_GAIN, source.gain));
	}
}


// set pitch of sound
static void setPitch(struct SaslSoundCallbacks *s, int sampleId, int pitch)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	alCheck(alSourcef(source, AL_PITCH, (float)pitch / 1000.0));
}


/// Rewind sample to beginning
static void rewindSound(struct SaslSoundCallbacks *s, int sampleId)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	alCheck(alSourceRewind(source));
}


// Test if sample playing now
static int isPlaying(struct SaslSoundCallbacks *s, int sampleId)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return 0;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	ALenum state;
	alCheck(alGetSourcei(source, AL_SOURCE_STATE, &state));
	return (state == AL_PLAYING);
}


// set gain of all sound
static void setMasterGain(struct SaslSoundCallbacks *s, int gain)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	if ((!sound) || (!sound->devices[0]) || (sound->contexts.empty())) {
		return;
	}

	for (std::vector<ALCcontext*>::iterator con_it = sound->contexts.begin();
		con_it != sound->contexts.end(); ++con_it) {

		ContextChanger changer(*con_it);
		alCheck(alListenerf(AL_GAIN, (float)gain / 1000.0));
	}
}


// Set sample location, outside, inside or both
static void setEnv(struct SaslSoundCallbacks *s, int sampleId, int scene)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint srcId = getSource(s, sampleId);
	if ((ALuint)-1 == srcId) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));

	Source &source = sound->sources[sampleId];
	source.scene = scene;

	if (sound->scene & source.scene) {
		alCheck(alSourcef(srcId, AL_GAIN, source.gain));
	} else {
		alCheck(alSourcef(srcId, AL_GAIN, 0));
	}
}


// Get sample location, outside, inside or both
static int getEnv(struct SaslSoundCallbacks *s, int sampleId)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint srcId = getSource(s, sampleId);
	if ((ALuint)-1 == srcId) {
		return SOUND_EVERYWHERE;
	}

	Source &source = sound->sources[sampleId];
	return source.scene;
}


/// Set sample position
static void setPosition(struct SaslSoundCallbacks *s,
	int sampleId, float x, float y, float z)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	alCheck(alSource3f(source, AL_POSITION, x, y, z));
}


// get sample position
static void getPosition(struct SaslSoundCallbacks *s,
	int sampleId, float *x, float *y, float *z)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	float position[3];

	ContextChanger changer(sound->getContextBySample(sampleId));
	alCheck(alGetSourcefv(source, AL_POSITION, position));
	if (x) *x = position[0];
	if (y) *y = position[1];
	if (z) *z = position[2];
}


/// Set sample direction
static void setDirection(struct SaslSoundCallbacks *s,
	int sampleId, float x, float y, float z)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	alCheck(alSource3f(source, AL_DIRECTION, x, y, z));
}


// Get sample direction
static void getDirection(struct SaslSoundCallbacks *s,
	int sampleId, float *x, float *y, float *z)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	float position[3];

	ContextChanger changer(sound->getContextBySample(sampleId));
	alCheck(alGetSourcefv(source, AL_DIRECTION, position));
	if (x) *x = position[0];
	if (y) *y = position[1];
	if (z) *z = position[2];
}


// Set maximum sample distance
static void setMaxDistance(struct SaslSoundCallbacks *s, int sampleId,
	float distance)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	alCheck(alSourcef(source, AL_MAX_DISTANCE, distance));
}

// Set rolloff factor
static void setRolloff(struct SaslSoundCallbacks *s, int sampleId,
	float rolloff)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	alCheck(alSourcef(source, AL_ROLLOFF_FACTOR, rolloff));
}


// Set reference distance
static void setRefDistance(struct SaslSoundCallbacks *s, int sampleId,
	float distance)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	alCheck(alSourcef(source, AL_REFERENCE_DISTANCE, distance));
}

// Set sound cone parameters
static void setCone(struct SaslSoundCallbacks *s, int sampleId,
	float outerGain, float innerAngle, float outerAngle)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	alCheck(alSourcef(source, AL_CONE_OUTER_GAIN, outerGain));
	alCheck(alSourcef(source, AL_CONE_INNER_ANGLE, innerAngle));
	alCheck(alSourcef(source, AL_CONE_OUTER_ANGLE, outerAngle));
}


// get sound cone parameters
static void getCone(struct SaslSoundCallbacks *s, int sampleId,
	float *outerGain, float *innerAngle, float *outerAngle)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	if (outerGain)
		alCheck(alGetSourcef(source, AL_CONE_OUTER_GAIN, outerGain));
	if (innerAngle)
		alCheck(alGetSourcef(source, AL_CONE_INNER_ANGLE, innerAngle));
	if (outerAngle)
		alCheck(alGetSourcef(source, AL_CONE_OUTER_ANGLE, outerAngle));
}


// set sample relative listener flag
static void setRelative(struct SaslSoundCallbacks *s, int sampleId,
	int relative)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	alCheck(alSourcei(source, AL_SOURCE_RELATIVE, relative));
}


// get sample relative listener flag
static int getRelative(struct SaslSoundCallbacks *s, int sampleId)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	ALuint source = getSource(s, sampleId);
	if ((ALuint)-1 == source) {
		return 0;
	}

	ContextChanger changer(sound->getContextBySample(sampleId));
	int res = 0;
	alCheck(alGetSourcei(source, AL_SOURCE_RELATIVE, &res));
	return res;
}


// set listener location, outside or inside
static void setListenerEnv(struct SaslSoundCallbacks *s, int scene)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	if ((!sound) || (!sound->devices[0]) || (sound->contexts.empty())) {
		return;
	}
	if (sound->scene == scene) {
		return;
	}

	for (std::vector<ALCcontext*>::iterator con_it = sound->contexts.begin();
		con_it != sound->contexts.end(); ++con_it) {

		ContextChanger changer(*con_it);
		std::size_t context_index = std::distance(sound->contexts.begin(), con_it);

		sound->scene = scene;

		for (std::map<int, Source>::iterator i = sound->sources.begin();
			i != sound->sources.end(); ++i)
		{
			if ((std::size_t)(((*i).first - 1) / sound->maxSourcesPerContext) == context_index) {
				Source &source = (*i).second;
				if (scene & source.scene) {
					alCheck(alSourcef(source.id, AL_GAIN, source.gain));
				} else {
					alCheck(alSourcef(source.id, AL_GAIN, 0));
				}
			}
		}
	}
}


// set listener position
static void setListenerPosition(struct SaslSoundCallbacks *s,
	float x, float y, float z)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	if ((!sound) || (!sound->devices[0]) || (sound->contexts.empty())) {
		return;
	}

	for (std::vector<ALCcontext*>::iterator con_it = sound->contexts.begin();
		con_it != sound->contexts.end(); ++con_it) {

		ContextChanger changer(*con_it);
		alCheck(alListener3f(AL_POSITION, x, y, z));
	}
}


// get listener position
static void getListenerPosition(struct SaslSoundCallbacks *s,
	float *x, float *y, float *z)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	if ((!sound) || (!sound->devices[0]) || (sound->contexts.empty())) {
		return;
	}

	ContextChanger changer(sound->contexts[0]);

	float position[3];

	alCheck(alGetListenerfv(AL_POSITION, position));
	if (x) *x = position[0];
	if (y) *y = position[1];
	if (z) *z = position[2];
}

// set listener orientation
static void setListenerOrientation(struct SaslSoundCallbacks *s,
	float x, float y, float z, float ux, float uy, float uz)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	if ((!sound) || (!sound->devices[0]) || (sound->contexts.empty())) {
		return;
	}

	for (std::vector<ALCcontext*>::iterator con_it = sound->contexts.begin();
		con_it != sound->contexts.end(); ++con_it) {

		ContextChanger changer(*con_it);
		float orientation[] = { x, y, z, ux, uy, uz };
		alCheck(alListenerfv(AL_ORIENTATION, orientation));
	}
}


// get listener orientation
static void getListenerOrientation(struct SaslSoundCallbacks *s,
	float *x, float *y, float *z,
	float *ux, float *uy, float *uz)
{
	SaslAlSound *sound = (SaslAlSound*)s;
	if ((!sound) || (!sound->devices[0]) || (sound->contexts.empty())) {
		return;
	}

	ContextChanger changer(sound->contexts[0]);

	float orientation[6];

	alCheck(alGetListenerfv(AL_ORIENTATION, orientation));
	if (x) *x = orientation[0];
	if (y) *y = orientation[1];
	if (z) *z = orientation[2];
	if (ux) *ux = orientation[3];
	if (uy) *uy = orientation[4];
	if (uz) *uz = orientation[5];
}


// nothing needed
static void update(struct SaslSoundCallbacks *sound)
{
}

static void getContextsState(struct SaslSoundCallbacks *s,
	int* sources, int* sources_context_limit) {

	SaslAlSound *sound = (SaslAlSound*)s;
	if ((!sound) || (!sound->devices[0]) || (sound->contexts.empty())) {
		return;
	}

	if (sources) {
		*sources = sound->curSourceNumber;
	}

	if (sources_context_limit) {
		*sources_context_limit = sound->maxSourcesPerContext;
	}
}

// initialize sound engined
SaslAlSound* sasl_init_al_sound(SASL sasl)
{
	SaslAlSound* sound = new SaslAlSound;

	struct SaslSoundCallbacks cb = { loadSound, loadSoundReversed, unloadSound, playSound,
		stopSound, setGain, setPitch, rewindSound, isPlaying, getSamplePlayingLeft,
		setEnv, getEnv, setPosition, getPosition, setDirection, getDirection,
		setMaxDistance, setRolloff, setRefDistance, setCone, getCone,
		setRelative, getRelative, setListenerEnv, setListenerPosition,
		getListenerPosition, setListenerOrientation, getListenerOrientation,
		setMasterGain, update, getContextsState };
	sound->callbacks = cb;
#if defined(WINDOWS) || defined(LIN)
	sound->devices.push_back(alcOpenDevice(NULL));
#else    
	sound->devices.push_back(alcOpenDevice("Generic Software"));
#endif // WINDOWS    

	alcCheck(sound->contexts.push_back(alcCreateContext(sound->devices[0], NULL)), sound->devices[0]);
	sound->curSourceNumber = 0;
	sound->sasl = sasl;
	sound->scene = 1;

	ALCint maxMonoPerContext, maxStereoPerContext;

	{
		ContextChanger changer(sound->contexts[0]);
		alcCheck(alcGetIntegerv(sound->devices[0], ALC_MONO_SOURCES, 1, &maxMonoPerContext), sound->devices[0]);
		alcCheck(alcGetIntegerv(sound->devices[0], ALC_STEREO_SOURCES, 1, &maxStereoPerContext), sound->devices[0]);
	}

	sound->maxSourcesPerContext = maxMonoPerContext;
#if !defined(WINDOWS) && !defined(LIN)
	sound->maxSourcesPerContext = 255;
#endif

	sasl_set_sound_engine(sasl, &sound->callbacks);
	ALChecker::setSASL(sasl);

	return sound;
}


// destroy sound engine
void sasl_done_al_sound(SaslAlSound *sound)
{
	if ((!sound) || (!sound->devices[0]) || (sound->contexts.empty())) {
		return;
	}

	for (std::vector<ALCcontext*>::iterator con_it = sound->contexts.begin();
		con_it != sound->contexts.end(); ++con_it)
	{

		ContextChanger changer(*con_it);
		std::size_t context_index = std::distance(sound->contexts.begin(), con_it);

		for (std::map<int, Source>::iterator i = sound->sources.begin();
			i != sound->sources.end(); ++i)
		{
			if ((std::size_t)(((*i).first - 1) / sound->maxSourcesPerContext) == context_index) {
				ALuint source = (*i).second.id;
				alCheck(alDeleteSources(1, &source));
				if ((*i).second.timer) {
					delete (*i).second.timer;
					(*i).second.timer = NULL;
				}
			}
		}
	}

	for (std::map<std::string, Buffer>::iterator i = sound->buffers.begin();
		i != sound->buffers.end(); ++i)
	{
		ContextChanger changer(sound->contexts[0]);

		ALuint buffer = (*i).second.id;
		alCheck(alDeleteBuffers(1, &buffer));
	}

	sound->sources.clear();
	sound->buffers.clear();

	for (std::vector<ALCcontext*>::iterator con_it = sound->contexts.begin();
		con_it != sound->contexts.end(); ++con_it)
	{
		if (alcGetCurrentContext() == *con_it) {
			alcMakeContextCurrent(NULL);
		}
		alcDestroyContext(*con_it);
		*con_it = NULL;
	}

	sound->contexts.clear();

	for (std::vector<ALCdevice*>::iterator dev_it = sound->devices.begin();
		dev_it != sound->devices.end(); ++dev_it) {

		alcCloseDevice(*dev_it);
		*dev_it = NULL;
	}

	sound->devices.clear();

	sasl_set_sound_engine(sound->sasl, NULL);
}

