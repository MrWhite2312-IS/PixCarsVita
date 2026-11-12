/*
 * PixCarsVita - video playback. See movie.h.
 */
#include "movie.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <vitaGL.h>

static SceAvPlayerHandle g_player = -1;
static GLuint g_tex;
static volatile int g_audio_on;
static SceUID g_audio_thread = -1;
static int g_have_frame;

static void *mem_alloc(void *p, uint32_t align, uint32_t size)
{
    (void)p;
    return memalign(align, size);
}

static void mem_free(void *p, void *ptr)
{
    (void)p;
    free(ptr);
}

/* video frames must be GPU-mapped: CDRAM blocks (256 KiB granularity) mapped for GXM */
static void *gpu_alloc(void *p, uint32_t align, uint32_t size)
{
    (void)p;
    (void)align;
    size = (size + 0x3FFFF) & ~0x3FFFFu;
    SceUID uid = sceKernelAllocMemBlock("pixcars_video", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
    if (uid < 0)
        return NULL;
    void *addr = NULL;
    sceKernelGetMemBlockBase(uid, &addr);
    sceGxmMapMemory(addr, size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    return addr;
}

static void gpu_free(void *p, void *ptr)
{
    (void)p;
    glFinish(); /* the GPU may still be sampling the frame */
    sceGxmUnmapMemory(ptr);
    sceKernelFreeMemBlock(sceKernelFindMemBlockByAddr(ptr, 0));
}

static int audio_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    static int16_t silence[2 * 1024];
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
        return 0;
    while (g_audio_on) {
        SceAvPlayerFrameInfo f;
        if (g_player >= 0 && sceAvPlayerIsActive(g_player) && sceAvPlayerGetAudioData(g_player, &f))
            sceAudioOutOutput(port, f.pData);
        else
            sceAudioOutOutput(port, silence);
    }
    sceAudioOutReleasePort(port);
    return 0;
}

int movie_start(const char *path)
{
    movie_stop();
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0)
        return -1;
    static int loaded;
    if (!loaded) {
        int m = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        if (m < 0)
            return m;
        loaded = 1;
    }
    SceAvPlayerInitData init;
    memset(&init, 0, sizeof init);
    init.memoryReplacement.allocate = mem_alloc;
    init.memoryReplacement.deallocate = mem_free;
    init.memoryReplacement.allocateTexture = gpu_alloc;
    init.memoryReplacement.deallocateTexture = gpu_free;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = 2;
    init.autoStart = 1;
    g_player = sceAvPlayerInit(&init);
    if (g_player < 0)
        return g_player < -2 ? g_player : -2; /* SCE error code for the log */
    sceAvPlayerAddSource(g_player, path);
    if (!g_tex) {
        /* a texture whose GXM descriptor is re-pointed at each decoded frame */
        static uint32_t dummy[64];
        glGenTextures(1, &g_tex);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, dummy);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    g_have_frame = 0;
    g_audio_on = 1;
    g_audio_thread = sceKernelCreateThread("pixcars_movie_audio", audio_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (g_audio_thread >= 0)
        sceKernelStartThread(g_audio_thread, 0, NULL);
    return 0;
}

int movie_draw(void)
{
    if (g_player < 0)
        return 0;
    if (!sceAvPlayerIsActive(g_player)) {
        movie_stop();
        return 0;
    }
    SceAvPlayerFrameInfo f;
    glBindTexture(GL_TEXTURE_2D, g_tex);
    if (sceAvPlayerGetVideoData(g_player, &f)) {
        sceGxmTextureInitLinear(vglGetGxmTexture(GL_TEXTURE_2D), f.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
                                f.details.video.width, f.details.video.height, 0);
        g_have_frame = 1;
    }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (g_have_frame) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, 1, 1, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_FOG);
        glEnable(GL_TEXTURE_2D);
        const float v[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
        const float t[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
        glColor4f(1, 1, 1, 1);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(2, GL_FLOAT, 0, v);
        glTexCoordPointer(2, GL_FLOAT, 0, t);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glEnable(GL_DEPTH_TEST);
    }
    return 1;
}

void movie_stop(void)
{
    if (g_audio_thread >= 0) {
        g_audio_on = 0;
        sceKernelWaitThreadEnd(g_audio_thread, NULL, NULL);
        sceKernelDeleteThread(g_audio_thread);
        g_audio_thread = -1;
    }
    if (g_player >= 0) {
        sceAvPlayerStop(g_player);
        sceAvPlayerClose(g_player);
        g_player = -1;
    }
}
