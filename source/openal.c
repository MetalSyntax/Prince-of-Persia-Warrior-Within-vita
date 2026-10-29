/**
 * @file openal.c
 * @brief OpenAL 1.1 audio subsystem bindings for PS Vita.
 */
#include <AL/al.h>
#include <AL/alc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imports.h"
#include "log.h"

static ALCdevice *s_dev = NULL;
static ALCcontext *s_ctx = NULL;

static ALCdevice *oal_alcOpenDevice(const ALCchar *devicename) {
    l_info("[AL] alcOpenDevice(%s)", devicename ? devicename : "default");
    if (!s_dev) {
        s_dev = alcOpenDevice(devicename);
        if (!s_dev) {
            l_warn("[AL] alcOpenDevice returned NULL, using dummy handle");
            s_dev = (ALCdevice *)0x00A10001;
        }
    }
    return s_dev;
}

static ALCboolean oal_alcCloseDevice(ALCdevice *device) {
    l_info("[AL] alcCloseDevice(%p)", device);
    if (device == s_dev && s_dev != (ALCdevice *)0x00A10001) {
        alcCloseDevice(device);
        s_dev = NULL;
    }
    return ALC_TRUE;
}

static ALCcontext *oal_alcCreateContext(ALCdevice *device, const ALCint *attrlist) {
    l_info("[AL] alcCreateContext(device=%p)", device);
    if (device != (ALCdevice *)0x00A10001) {
        s_ctx = alcCreateContext(device, attrlist);
    }
    if (!s_ctx) {
        s_ctx = (ALCcontext *)0x00A10002;
    }
    return s_ctx;
}

static ALCboolean oal_alcMakeContextCurrent(ALCcontext *context) {
    l_info("[AL] alcMakeContextCurrent(context=%p)", context);
    if (context != (ALCcontext *)0x00A10002) {
        return alcMakeContextCurrent(context);
    }
    return ALC_TRUE;
}

static void oal_alcProcessContext(ALCcontext *context) {
    if (context != (ALCcontext *)0x00A10002) {
        alcProcessContext(context);
    }
}

static void oal_alcSuspendContext(ALCcontext *context) {
    if (context != (ALCcontext *)0x00A10002) {
        alcSuspendContext(context);
    }
}

static void oal_alcDestroyContext(ALCcontext *context) {
    l_info("[AL] alcDestroyContext(context=%p)", context);
    if (context != (ALCcontext *)0x00A10002) {
        alcDestroyContext(context);
    }
    if (s_ctx == context)
        s_ctx = NULL;
}

static ALCcontext *oal_alcGetCurrentContext(void) {
    return s_ctx ? s_ctx : alcGetCurrentContext();
}

static ALCdevice *oal_alcGetContextsDevice(ALCcontext *context) {
    return s_dev ? s_dev : alcGetContextsDevice(context);
}

static ALCenum oal_alcGetError(ALCdevice *device) {
    if (device == (ALCdevice *)0x00A10001)
        return ALC_NO_ERROR;
    return alcGetError(device);
}

static void oal_alSourcef(ALuint source, ALenum param, float value) {
    alSourcef(source, param, value);
}

#define F(name, fn) {"_" #name, (uintptr_t)(fn), 0}
#define D(name)     {"_" #name, (uintptr_t)(name), 0}

const import_entry openal_imports[] = {
    D(alBufferData),
    D(alDeleteBuffers),
    D(alDeleteSources),
    D(alGenBuffers),
    D(alGenSources),
    D(alGetError),
    D(alGetSourcef),
    D(alGetSourcei),
    D(alListenerfv),
    D(alSourcePause),
    D(alSourcePlay),
    D(alSourceQueueBuffers),
    D(alSourceStop),
    D(alSourceUnqueueBuffers),
    F(alSourcef, oal_alSourcef),
    D(alSourcefv),
    D(alSourcei),
    F(alcCloseDevice, oal_alcCloseDevice),
    F(alcCreateContext, oal_alcCreateContext),
    F(alcDestroyContext, oal_alcDestroyContext),
    F(alcGetContextsDevice, oal_alcGetContextsDevice),
    F(alcGetCurrentContext, oal_alcGetCurrentContext),
    F(alcGetError, oal_alcGetError),
    F(alcMakeContextCurrent, oal_alcMakeContextCurrent),
    F(alcOpenDevice, oal_alcOpenDevice),
    F(alcProcessContext, oal_alcProcessContext),
    F(alcSuspendContext, oal_alcSuspendContext),
};

const int openal_imports_count = sizeof(openal_imports) / sizeof(openal_imports[0]);
