#ifndef PS5_NULL_EFX_H
#define PS5_NULL_EFX_H
#include "al.h"
typedef void (*LPALGENEFFECTS)(ALsizei,ALuint*);
typedef void (*LPALDELETEEFFECTS)(ALsizei,const ALuint*);
typedef void (*LPALEFFECTI)(ALuint,ALenum,ALint);
typedef void (*LPALEFFECTF)(ALuint,ALenum,ALfloat);
typedef void (*LPALGENAUXILIARYEFFECTSLOTS)(ALsizei,ALuint*);
typedef void (*LPALDELETEAUXILIARYEFFECTSLOTS)(ALsizei,const ALuint*);
typedef void (*LPALAUXILIARYEFFECTSLOTI)(ALuint,ALenum,ALint);
#endif
