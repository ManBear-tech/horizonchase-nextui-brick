/*
 * opensles_shim.h -- OpenSL ES to SDL2 audio bridge
 */

#ifndef __OPENSLES_SHIM_H__
#define __OPENSLES_SHIM_H__

#include <stdint.h>

typedef uint32_t SLresult;
typedef uint32_t SLuint32;
typedef int32_t SLint32;
typedef uint16_t SLuint16;
typedef int16_t SLint16;
typedef uint8_t SLuint8;
typedef int8_t SLint8;
typedef int32_t SLmillibel;
typedef uint32_t SLmillisecond;
typedef uint32_t SLBoolean;

#define SL_RESULT_SUCCESS ((SLresult)0x00000000)
#define SL_RESULT_RESOURCE_ERROR ((SLresult)0x0000000D)
#define SL_BOOLEAN_TRUE ((SLBoolean)0x00000001)
#define SL_BOOLEAN_FALSE ((SLBoolean)0x00000000)

#define SL_PLAYSTATE_STOPPED ((SLuint32)0x00000001)
#define SL_PLAYSTATE_PAUSED ((SLuint32)0x00000002)
#define SL_PLAYSTATE_PLAYING ((SLuint32)0x00000003)
#define SL_TIME_UNKNOWN ((SLmillisecond)0xFFFFFFFF)

#define SL_PLAYEVENT_HEADATEND ((SLuint32)0x00000001)
#define SL_PLAYEVENT_HEADATMARKER ((SLuint32)0x00000002)
#define SL_PLAYEVENT_HEADATNEWPOS ((SLuint32)0x00000004)
#define SL_PLAYEVENT_HEADMOVING ((SLuint32)0x00000008)
#define SL_PLAYEVENT_HEADSTALLED ((SLuint32)0x00000010)

#define SL_DATAFORMAT_PCM 2
#define SL_DATALOCATOR_BUFFERQUEUE 0x800007BD
#define SL_DATALOCATOR_OUTPUTMIX 4

typedef const void *SLInterfaceID;

extern const SLInterfaceID sl_IID_ENGINE;
extern const SLInterfaceID sl_IID_PLAY;
extern const SLInterfaceID sl_IID_VOLUME;
extern const SLInterfaceID sl_IID_BUFFERQUEUE;
extern const SLInterfaceID sl_IID_EFFECTSEND;
extern const SLInterfaceID sl_IID_ENGINECAPABILITIES;
extern const SLInterfaceID sl_IID_ANDROIDCONFIGURATION;
extern const SLInterfaceID sl_IID_ENVIRONMENTALREVERB;

SLresult slCreateEngine_shim(void **pEngine, SLuint32 numOptions,
                              const void *pEngineOptions,
                              SLuint32 numInterfaces,
                              const SLInterfaceID *pInterfaceIds,
                              const SLBoolean *pInterfaceRequired);

void opensles_shim_pump_callbacks(void);
/* 1 = SDL audio aberto (FMOD entrou no caminho OpenSL) — gate do feeder fmodProcess */
int opensles_shim_engine_active(void);

#endif
