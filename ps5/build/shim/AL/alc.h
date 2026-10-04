#ifndef PS5_NULL_ALC_H
#define PS5_NULL_ALC_H
#include "al.h"
typedef struct ALCdevice_s ALCdevice; typedef struct ALCcontext_s ALCcontext;
typedef char ALCboolean; typedef int ALCint; typedef int ALCenum; typedef char ALCchar;
#define ALC_NO_ERROR 0
#define ALC_INVALID_DEVICE 0xA001
#define ALC_INVALID_CONTEXT 0xA002
#define ALC_INVALID_ENUM 0xA003
#define ALC_INVALID_VALUE 0xA004
#define ALC_OUT_OF_MEMORY 0xA005
#define ALC_FREQUENCY 0x1007
#define ALC_DEVICE_SPECIFIER 0x1005
#define ALC_MAX_AUXILIARY_SENDS 0x20003
#define ALC_EXT_EFX_NAME "ALC_EXT_EFX"
#ifdef __cplusplus
extern "C" {
#endif
ALCdevice*alcOpenDevice(const ALCchar*n);
ALCboolean alcCloseDevice(ALCdevice*d);
ALCcontext*alcCreateContext(ALCdevice*d,const ALCint*a);
void alcDestroyContext(ALCcontext*c);
ALCboolean alcMakeContextCurrent(ALCcontext*c);
ALCenum alcGetError(ALCdevice*d);
const ALCchar*alcGetString(ALCdevice*d,ALCenum p);
void alcGetIntegerv(ALCdevice*d,ALCenum p,ALsizei n,ALCint*v);
ALCboolean alcIsExtensionPresent(ALCdevice*d,const ALCchar*n);
#ifdef __cplusplus
}
#endif
#endif
