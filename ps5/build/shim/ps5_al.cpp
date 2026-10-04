// Minimal software OpenAL for PS5: just the subset REDRIVER2 uses (static-buffer sources, gain/pitch/pan/loop,
// loop points, simple queue). Mixes in a thread and plays through sceAudioOut. No EFX/reverb.
#include "AL/alc.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <vector>
#include <deque>
#include <map>
#include <time.h>
static double NowMs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }

extern "C" {
int sceAudioOutInit(void);
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int* userId);
int sceAudioOutOpen(int userId, int type, int index, unsigned len, unsigned freq, unsigned param);
int sceAudioOutOutput(int handle, const void* buf);
void PsyX_Log(const char* fmt, ...);
}

namespace {
const int kRate = 48000;
const int kGrain = 512;

struct Buf { int format = 0, freq = 44100; std::vector<int16_t> pcm; int channels = 1; int loopStart = 0, loopEnd = -1; };
struct Src {
	int state = AL_INITIAL; float gain = 1, pitch = 1, px = 0; bool looping = false;
	ALuint buffer = 0;                 // static buffer
	std::deque<ALuint> queue; int processed = 0; // queued buffers (front = playing)
	double pos = 0;                    // position in frames of the current buffer
};

pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
std::map<ALuint, Buf> g_bufs;
std::map<ALuint, Src> g_srcs;
ALuint g_nextBuf = 1, g_nextSrc = 1;
bool g_thread = false;
volatile int g_audioOk = 0;
volatile int g_starved = 0;
double g_qMin = 1e9, g_qMax = 0, g_qSum = 0; int g_qN = 0, g_qBufs = 0; double g_bufMs = 0; int g_qReal = 0;

inline int frames(const Buf& b) { return (int)(b.pcm.size() / b.channels); }

void* MixThread(void*)
{
	int r0 = sceAudioOutInit();
	int r1 = sceUserServiceInitialize(NULL);
	int user = -1;
	int r2 = sceUserServiceGetInitialUser(&user);
	PsyX_Log("audio: init=%x userInit=%x getUser=%x user=%d\n", r0, r1, r2, user);
	int h = -1;
	const int cands[] = { r2 == 0 ? user : 255, 255, 0, 1 };
	for (int i = 0; i < 4; i++)
	{
		h = sceAudioOutOpen(cands[i], 0 /*MAIN*/, 0, kGrain, kRate, 1 /*S16 stereo*/);
		PsyX_Log("audio: open user=%d -> %x\n", cands[i], h);
		if (h >= 0)
			break;
	}
	if (h < 0)
		return NULL;
	g_audioOk = 1;
	int16_t out[kGrain * 2];
	int32_t acc[kGrain * 2];
	double lastEnd = NowMs(), maxMix = 0, maxPeriod = 0, maxOut = 0; int iter = 0;
	for (;;)
	{
		double t0 = NowMs();
		memset(acc, 0, sizeof(acc));
		pthread_mutex_lock(&g_mx);
		for (auto& kv : g_srcs)
		{
			Src& s = kv.second;
			if (s.state != AL_PLAYING)
				continue;
			// Streaming sources (queued buffers, e.g. FMV audio): the producer's clock differs slightly from ours.
			// Nudge the playback speed (+-2%) to keep about 250 ms queued instead of running dry (audible gaps).
			double speed = 1.0;
			if (!s.queue.empty())
			{
				double remaining = 0;
				for (int q = s.processed; q < (int)s.queue.size(); q++)
				{
					auto bi = g_bufs.find(s.queue[q]);
					if (bi != g_bufs.end() && bi->second.freq > 0)
						remaining += frames(bi->second) * 1000.0 / bi->second.freq;
				}
				if (remaining < g_qMin) g_qMin = remaining; if (remaining > g_qMax) g_qMax = remaining; g_qSum += remaining; g_qN++; g_qBufs = (int)s.queue.size() - s.processed; g_qReal = (int)s.queue.size();
				{ auto bi = g_bufs.find(s.queue[s.processed < (int)s.queue.size() ? s.processed : 0]); if (bi != g_bufs.end()) g_bufMs = frames(bi->second) * 1000.0 / bi->second.freq; }
				double err = (remaining - 250.0) / 250.0;
				if (err > 1) err = 1; else if (err < -1) err = -1;
				speed = 1.0 + err * 0.02;
			}
			const float lg = s.gain * (s.px > 0 ? 1.0f - s.px : 1.0f);
			const float rg = s.gain * (s.px < 0 ? 1.0f + s.px : 1.0f);
			for (int i = 0; i < kGrain && s.state == AL_PLAYING; i++)
			{
				if (!s.queue.empty() && s.processed >= (int)s.queue.size()) { g_starved++; break; } // starved: silent until more is queued
				ALuint id = s.queue.empty() ? s.buffer : s.queue[s.processed];
				auto it = g_bufs.find(id);
				if (id == 0 || it == g_bufs.end() || it->second.pcm.empty()) { s.state = AL_STOPPED; break; }
				Buf& b = it->second;
				int n = frames(b);
				int end = (s.looping && s.queue.empty() && b.loopEnd > b.loopStart) ? b.loopEnd : n;
				if (end > n) end = n;
				int i0 = (int)s.pos;
				if (i0 >= end)
				{
					if (s.looping && s.queue.empty()) { s.pos = (b.loopEnd > b.loopStart) ? b.loopStart : 0; i0 = (int)s.pos; if (i0 >= end) { s.state = AL_STOPPED; break; } }
					else if (!s.queue.empty()) { if (s.processed < (int)s.queue.size()) { s.processed++; s.pos = 0; } if (s.processed >= (int)s.queue.size()) break; /* starved: stay PLAYING (silent) until the game queues more */ i--; continue; }
					else { s.state = AL_STOPPED; break; }
				}
				int i1 = i0 + 1 < end ? i0 + 1 : i0;
				float f = (float)(s.pos - i0);
				float l, r;
				if (b.channels == 1)
				{
					l = r = b.pcm[i0] * (1 - f) + b.pcm[i1] * f;
				}
				else
				{
					l = b.pcm[i0 * 2] * (1 - f) + b.pcm[i1 * 2] * f;
					r = b.pcm[i0 * 2 + 1] * (1 - f) + b.pcm[i1 * 2 + 1] * f;
				}
				acc[i * 2] += (int32_t)(l * lg);
				acc[i * 2 + 1] += (int32_t)(r * rg);
				s.pos += (double)s.pitch * speed * b.freq / kRate;
			}
		}
		pthread_mutex_unlock(&g_mx);
		for (int i = 0; i < kGrain * 2; i++)
		{
			int32_t v = acc[i];
			out[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
		}
		double t1 = NowMs();
		sceAudioOutOutput(h, out);
		double t2 = NowMs();
		if (t1 - t0 > maxMix) maxMix = t1 - t0;
		if (t2 - t1 > maxOut) maxOut = t2 - t1;
		if (t2 - lastEnd > maxPeriod) maxPeriod = t2 - lastEnd;
		lastEnd = t2;
	}
	return NULL;
}
}

extern "C" {
ALenum alGetError(void) { return 0; }
void alGenBuffers(ALsizei n, ALuint* b) { pthread_mutex_lock(&g_mx); for (int i = 0; i < n; i++) { b[i] = g_nextBuf++; g_bufs[b[i]]; } pthread_mutex_unlock(&g_mx); }
void alGenSources(ALsizei n, ALuint* b) { pthread_mutex_lock(&g_mx); for (int i = 0; i < n; i++) { b[i] = g_nextSrc++; g_srcs[b[i]]; } pthread_mutex_unlock(&g_mx); }
void alDeleteBuffers(ALsizei n, const ALuint* b) { pthread_mutex_lock(&g_mx); for (int i = 0; i < n; i++) g_bufs.erase(b[i]); pthread_mutex_unlock(&g_mx); }
void alDeleteSources(ALsizei n, const ALuint* b) { pthread_mutex_lock(&g_mx); for (int i = 0; i < n; i++) g_srcs.erase(b[i]); pthread_mutex_unlock(&g_mx); }
void alBufferData(ALuint id, ALenum f, const void* d, ALsizei size, ALsizei freq)
{
	pthread_mutex_lock(&g_mx);
	Buf& b = g_bufs[id];
	b.format = f; b.freq = freq; b.loopStart = 0; b.loopEnd = -1;
	b.channels = (f == AL_FORMAT_STEREO16 || f == AL_FORMAT_STEREO8) ? 2 : 1;
	if (f == AL_FORMAT_MONO16 || f == AL_FORMAT_STEREO16)
		b.pcm.assign((const int16_t*)d, (const int16_t*)d + size / 2);
	else
	{
		b.pcm.resize(size);
		for (int i = 0; i < size; i++) b.pcm[i] = (int16_t)((((const uint8_t*)d)[i] - 128) << 8);
	}
	pthread_mutex_unlock(&g_mx);
}
void alBufferiv(ALuint id, ALenum p, const ALint* v)
{
	pthread_mutex_lock(&g_mx);
	if (p == AL_LOOP_POINTS_SOFT) { g_bufs[id].loopStart = v[0]; g_bufs[id].loopEnd = v[1]; }
	pthread_mutex_unlock(&g_mx);
}
void alGetBufferi(ALuint id, ALenum p, ALint* v)
{
	pthread_mutex_lock(&g_mx);
	*v = (p == AL_SAMPLE_LENGTH_SOFT) ? frames(g_bufs[id]) : 0;
	pthread_mutex_unlock(&g_mx);
}
void alSourcei(ALuint id, ALenum p, ALint v)
{
	pthread_mutex_lock(&g_mx);
	Src& s = g_srcs[id];
	if (p == AL_BUFFER) { s.buffer = v; s.queue.clear(); s.processed = 0; s.pos = 0; if (s.state != AL_PLAYING) s.state = AL_INITIAL; }
	else if (p == AL_LOOPING) s.looping = v != 0;
	pthread_mutex_unlock(&g_mx);
}
void alSourcef(ALuint id, ALenum p, ALfloat v)
{
	pthread_mutex_lock(&g_mx);
	Src& s = g_srcs[id];
	if (p == AL_GAIN) s.gain = v; else if (p == AL_PITCH) s.pitch = v;
	pthread_mutex_unlock(&g_mx);
}
void alSource3f(ALuint id, ALenum p, ALfloat x, ALfloat, ALfloat)
{
	pthread_mutex_lock(&g_mx);
	if (p == AL_POSITION) g_srcs[id].px = x > 1 ? 1 : (x < -1 ? -1 : x);
	pthread_mutex_unlock(&g_mx);
}
void alSource3i(ALuint, ALenum, ALint, ALint, ALint) {}
void alGetSourcei(ALuint id, ALenum p, ALint* v)
{
	pthread_mutex_lock(&g_mx);
	Src& s = g_srcs[id];
	*v = p == AL_SOURCE_STATE ? s.state : p == AL_BUFFERS_PROCESSED ? (s.processed < (int)s.queue.size() ? s.processed : (int)s.queue.size())
	   : p == AL_BUFFERS_QUEUED ? (int)s.queue.size() : 0;
	pthread_mutex_unlock(&g_mx);
}
void alSourcePlay(ALuint id)
{
	pthread_mutex_lock(&g_mx);
	Src& s = g_srcs[id];
	if (s.state != AL_PAUSED) s.pos = 0;
	s.state = AL_PLAYING;
	pthread_mutex_unlock(&g_mx);
}
void alSourceStop(ALuint id) { pthread_mutex_lock(&g_mx); Src& s = g_srcs[id]; s.state = AL_STOPPED; pthread_mutex_unlock(&g_mx); }
void alSourcePause(ALuint id) { pthread_mutex_lock(&g_mx); Src& s = g_srcs[id]; if (s.state == AL_PLAYING) s.state = AL_PAUSED; pthread_mutex_unlock(&g_mx); }
void alSourceQueueBuffers(ALuint id, ALsizei n, const ALuint* b) { pthread_mutex_lock(&g_mx); Src& s = g_srcs[id]; for (int i = 0; i < n; i++) s.queue.push_back(b[i]); pthread_mutex_unlock(&g_mx); }
void alSourceUnqueueBuffers(ALuint id, ALsizei n, ALuint* b)
{
	pthread_mutex_lock(&g_mx);
	Src& s = g_srcs[id];
	for (int i = 0; i < n && !s.queue.empty(); i++) { b[i] = s.queue.front(); s.queue.pop_front(); if (s.processed > 0) s.processed--; }
	pthread_mutex_unlock(&g_mx);
}
void alListenerf(ALenum, ALfloat) {}
void alDistanceModel(ALenum) {}
void* alGetProcAddress(const ALchar*) { return NULL; }

ALCdevice* alcOpenDevice(const ALCchar*) { return (ALCdevice*)1; }
ALCboolean alcCloseDevice(ALCdevice*) { return 1; }
ALCcontext* alcCreateContext(ALCdevice*, const ALCint*) { return (ALCcontext*)1; }
void alcDestroyContext(ALCcontext*) {}
ALCboolean alcMakeContextCurrent(ALCcontext*)
{
	if (!g_thread)
	{
		g_thread = true;
		pthread_t t;
		pthread_create(&t, NULL, MixThread, NULL);
	}
	return 1;
}
ALCenum alcGetError(ALCdevice*) { return 0; }
const ALCchar* alcGetString(ALCdevice*, ALCenum) { return "PS5 AudioOut\0"; }
void alcGetIntegerv(ALCdevice*, ALCenum, ALsizei n, ALCint* v) { for (int i = 0; i < n; i++) v[i] = 0; }
ALCboolean alcIsExtensionPresent(ALCdevice*, const ALCchar*) { return 0; }
}
