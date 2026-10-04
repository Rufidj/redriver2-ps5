/* Null OpenAL shim for PS5: every call is a no-op. Audio backend comes later. */
#ifndef PS5_NULL_AL_H
#define PS5_NULL_AL_H
#include <stddef.h>
typedef char ALboolean; typedef char ALchar; typedef signed char ALbyte; typedef unsigned char ALubyte;
typedef short ALshort; typedef unsigned short ALushort; typedef int ALint; typedef unsigned int ALuint;
typedef int ALsizei; typedef int ALenum; typedef float ALfloat; typedef double ALdouble; typedef void ALvoid;
typedef long long ALint64SOFT; typedef unsigned long long ALuint64SOFT;
#define AL_NONE 0
#define AL_FALSE 0
#define AL_TRUE 1
#define AL_NO_ERROR 0
#define AL_INVALID_NAME 0xA001
#define AL_INVALID_ENUM 0xA002
#define AL_INVALID_VALUE 0xA003
#define AL_INVALID_OPERATION 0xA004
#define AL_OUT_OF_MEMORY 0xA005
#define AL_SOURCE_RELATIVE 0x202
#define AL_LOOPING 0x1007
#define AL_BUFFER 0x1009
#define AL_GAIN 0x100A
#define AL_PITCH 0x1003
#define AL_POSITION 0x1004
#define AL_SOURCE_STATE 0x1010
#define AL_PLAYING 0x1012
#define AL_PAUSED 0x1013
#define AL_STOPPED 0x1014
#define AL_BUFFERS_QUEUED 0x1015
#define AL_BUFFERS_PROCESSED 0x1016
#define AL_UNKNOWN 0x1002
#define AL_FORMAT_MONO8 0x1100
#define AL_FORMAT_MONO16 0x1101
#define AL_FORMAT_STEREO8 0x1102
#define AL_FORMAT_STEREO16 0x1103
#define AL_LOOP_POINTS_SOFT 0x2015
#define AL_SAMPLE_LENGTH_SOFT 0x200A
#define AL_SOURCE_RESAMPLER_SOFT 0x1219
#define AL_FILTER_NULL 0
#define AL_EFFECT_NULL 0
#define AL_EFFECTSLOT_NULL 0
#define AL_EFFECT_TYPE 0x8001
#define AL_EFFECT_REVERB 0x0001
#define AL_EFFECTSLOT_EFFECT 0x0001
#define AL_AUXILIARY_SEND_FILTER 0x20006
#define AL_REVERB_DENSITY 1
#define AL_REVERB_DIFFUSION 2
#define AL_REVERB_GAIN 3
#define AL_REVERB_GAINHF 4
#define AL_REVERB_DECAY_TIME 5
#define AL_REVERB_DECAY_HFRATIO 6
#define AL_REVERB_REFLECTIONS_GAIN 7
#define AL_REVERB_REFLECTIONS_DELAY 9
#define AL_REVERB_AIR_ABSORPTION_GAINHF 0x0013
#define AL_INVERSE_DISTANCE_CLAMPED 0xD002
#define AL_NONE_DISTANCE_MODEL 0xD000
#define AL_DISTANCE_MODEL 0xD000
#define AL_API
#define AL_APIENTRY
#define AL_VERSION_1_1 1
#define AL_INITIAL 0x1011
#ifdef __cplusplus
extern "C" {
#endif
ALenum alGetError(void);
void alGenBuffers(ALsizei n, ALuint*b);
void alGenSources(ALsizei n, ALuint*b);
void alDeleteBuffers(ALsizei n,const ALuint*b);
void alDeleteSources(ALsizei n,const ALuint*b);
void alBufferData(ALuint b,ALenum f,const void*d,ALsizei s,ALsizei fr);
void alBufferiv(ALuint b,ALenum p,const ALint*v);
void alGetBufferi(ALuint b,ALenum p,ALint*v);
void alSourcei(ALuint s,ALenum p,ALint v);
void alSourcef(ALuint s,ALenum p,ALfloat v);
void alSource3f(ALuint s,ALenum p,ALfloat a,ALfloat b,ALfloat c);
void alSource3i(ALuint s,ALenum p,ALint a,ALint b,ALint c);
void alGetSourcei(ALuint s,ALenum p,ALint*v);
void alSourcePlay(ALuint s);
void alSourceStop(ALuint s);
void alSourcePause(ALuint s);
void alSourceQueueBuffers(ALuint s,ALsizei n,const ALuint*b);
void alSourceUnqueueBuffers(ALuint s,ALsizei n,ALuint*b);
void alListenerf(ALenum p,ALfloat v);
void alDistanceModel(ALenum m);
void*alGetProcAddress(const ALchar*n);
#ifdef __cplusplus
}
#endif
#endif
