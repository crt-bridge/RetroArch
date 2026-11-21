/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_nvenc.c -- see groovy_nvenc.h. */
#include "groovy_nvenc.h"

#include <stdlib.h>
#include <string.h>

void groovy_nvenc_cfg_defaults(groovy_nvenc_cfg *cfg, unsigned width, unsigned height)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->width = width;
    cfg->height = height;
    cfg->fps_num = 60u;
    cfg->fps_den = 1u;
    cfg->bitrate_kbps = 20000u;
    cfg->intra_refresh = 1;
    cfg->refresh_period = 60u;
}

#ifndef _WIN32

/* NVENC is reached through the Windows driver only for now. */
groovy_nvenc *groovy_nvenc_open(const groovy_nvenc_cfg *cfg, const char **why)
{
    (void)cfg;
    if (why) *why = "NVENC is only wired on Windows";
    return NULL;
}
void groovy_nvenc_close(groovy_nvenc *e) { (void)e; }
int groovy_nvenc_encode(groovy_nvenc *e, const uint8_t *bgr888, int force_key,
                        const uint8_t **out, size_t *len, int *is_key, double *us)
{
    (void)e; (void)bgr888; (void)force_key; (void)out; (void)len; (void)is_key; (void)us;
    return -1;
}

#else /* _WIN32 */

#include <windows.h>
#include "nvenc/nvEncodeAPI.h"

/* The four CUDA driver calls NVENC needs to get a device, declared here so that no CUDA header is needed. */
typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef CUresult (*cuInit_fn)(unsigned int);
typedef CUresult (*cuDeviceGet_fn)(CUdevice *, int);
typedef CUresult (*cuCtxCreate_fn)(CUcontext *, unsigned int, CUdevice);
typedef CUresult (*cuCtxDestroy_fn)(CUcontext);
typedef NVENCSTATUS (NVENCAPI *create_instance_fn)(NV_ENCODE_API_FUNCTION_LIST *);
typedef NVENCSTATUS (NVENCAPI *max_version_fn)(uint32_t *);

struct groovy_nvenc {
    HMODULE                   cuda_dll, nv_dll;
    cuCtxDestroy_fn           ctx_destroy;
    CUcontext                 ctx;
    NV_ENCODE_API_FUNCTION_LIST fl;
    void                     *enc;
    NV_ENC_INPUT_PTR          in;
    NV_ENC_OUTPUT_PTR         bs;
    unsigned                  w, h;
    uint32_t                  frame_idx;
    uint8_t                  *out;
    size_t                    out_cap;
    LARGE_INTEGER             qpf;
};

void groovy_nvenc_close(groovy_nvenc *e)
{
    if (!e) return;
    if (e->enc) {
        if (e->in && e->fl.nvEncDestroyInputBuffer) e->fl.nvEncDestroyInputBuffer(e->enc, e->in);
        if (e->bs && e->fl.nvEncDestroyBitstreamBuffer) e->fl.nvEncDestroyBitstreamBuffer(e->enc, e->bs);
        if (e->fl.nvEncDestroyEncoder) e->fl.nvEncDestroyEncoder(e->enc);
    }
    if (e->ctx && e->ctx_destroy) e->ctx_destroy(e->ctx);
    if (e->nv_dll) FreeLibrary(e->nv_dll);
    if (e->cuda_dll) FreeLibrary(e->cuda_dll);
    free(e->out);
    free(e);
}

groovy_nvenc *groovy_nvenc_open(const groovy_nvenc_cfg *cfg, const char **why)
{
    groovy_nvenc *e;
    cuInit_fn cu_init;
    cuDeviceGet_fn cu_dev;
    cuCtxCreate_fn cu_create;
    create_instance_fn create;
    max_version_fn maxver;
    uint32_t drv_ver = 0;
    CUdevice dev = 0;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp;
    NV_ENC_PRESET_CONFIG pc;
    NV_ENC_CONFIG ec;
    NV_ENC_INITIALIZE_PARAMS ip;
    NV_ENC_CREATE_INPUT_BUFFER ib;
    NV_ENC_CREATE_BITSTREAM_BUFFER bb;
    NVENCSTATUS st;
    const char *dummy;
    unsigned long long bps;

    if (!why) why = &dummy;
    if (!cfg || cfg->width < 33u || cfg->height < 17u || cfg->width > 4096u || cfg->height > 4096u
        || cfg->fps_num == 0u || cfg->fps_den == 0u || cfg->bitrate_kbps == 0u) {
        *why = "bad encoder configuration";
        return NULL;
    }
    e = (groovy_nvenc *)calloc(1u, sizeof *e);
    if (!e) { *why = "out of memory"; return NULL; }
    e->w = cfg->width; e->h = cfg->height;
    QueryPerformanceFrequency(&e->qpf);

    e->cuda_dll = LoadLibraryExA("nvcuda.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!e->cuda_dll) { *why = "nvcuda.dll not found (no NVIDIA driver)"; goto fail; }
    e->nv_dll = LoadLibraryExA("nvEncodeAPI64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!e->nv_dll) { *why = "nvEncodeAPI64.dll not found (no NVENC)"; goto fail; }
    cu_init = (cuInit_fn)(void *)GetProcAddress(e->cuda_dll, "cuInit");
    cu_dev = (cuDeviceGet_fn)(void *)GetProcAddress(e->cuda_dll, "cuDeviceGet");
    cu_create = (cuCtxCreate_fn)(void *)GetProcAddress(e->cuda_dll, "cuCtxCreate_v2");
    e->ctx_destroy = (cuCtxDestroy_fn)(void *)GetProcAddress(e->cuda_dll, "cuCtxDestroy_v2");
    create = (create_instance_fn)(void *)GetProcAddress(e->nv_dll, "NvEncodeAPICreateInstance");
    maxver = (max_version_fn)(void *)GetProcAddress(e->nv_dll, "NvEncodeAPIGetMaxSupportedVersion");
    if (!cu_init || !cu_dev || !cu_create || !e->ctx_destroy || !create || !maxver) { *why = "NVENC entry points missing"; goto fail; }

    /* the driver must speak at least the API version this file was written against */
    if (maxver(&drv_ver) != NV_ENC_SUCCESS
        || drv_ver < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) {
        *why = "the NVIDIA driver is older than the NVENC API of this build";
        goto fail;
    }
    if (cu_init(0) != 0 || cu_dev(&dev, 0) != 0 || cu_create(&e->ctx, 0, dev) != 0) { *why = "no CUDA device"; goto fail; }

    memset(&e->fl, 0, sizeof e->fl);
    e->fl.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (create(&e->fl) != NV_ENC_SUCCESS) { *why = "NvEncodeAPICreateInstance failed"; goto fail; }

    memset(&sp, 0, sizeof sp);
    sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sp.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sp.device = e->ctx;
    sp.apiVersion = NVENCAPI_VERSION;
    if (e->fl.nvEncOpenEncodeSessionEx(&sp, &e->enc) != NV_ENC_SUCCESS) { e->enc = NULL; *why = "cannot open an NVENC session"; goto fail; }

    memset(&pc, 0, sizeof pc);
    pc.version = NV_ENC_PRESET_CONFIG_VER;
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    st = e->fl.nvEncGetEncodePresetConfigEx(e->enc, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID,
                                            NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &pc);
    if (st != NV_ENC_SUCCESS) { *why = "no P1 low-latency preset for H.264"; goto fail; }
    ec = pc.presetCfg;
    ec.version = NV_ENC_CONFIG_VER;
    ec.profileGUID = NV_ENC_H264_PROFILE_HIGH_444_GUID;
    ec.gopLength = NVENC_INFINITE_GOPLENGTH;          /* keyframes only on request: a new mode, a lost frame */
    ec.frameIntervalP = 1;                            /* no B frames: a B frame costs at least one frame of delay */
    bps = (unsigned long long)cfg->bitrate_kbps * 1000ull;
    ec.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    ec.rcParams.averageBitRate = (uint32_t)bps;
    ec.rcParams.maxBitRate = (uint32_t)bps;
    ec.rcParams.vbvBufferSize = (uint32_t)(bps * cfg->fps_den / cfg->fps_num);   /* one frame, the Moonlight model */
    ec.rcParams.vbvInitialDelay = ec.rcParams.vbvBufferSize;
    ec.rcParams.enableLookahead = 0;
    ec.rcParams.zeroReorderDelay = 1;
    ec.encodeCodecConfig.h264Config.chromaFormatIDC = 3;       /* 4:4:4: the pixel art keeps its colour */
    ec.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    ec.encodeCodecConfig.h264Config.repeatSPSPPS = 1;          /* the parameter sets travel with every keyframe */
    if (cfg->intra_refresh && cfg->refresh_period > 1u) {
        ec.encodeCodecConfig.h264Config.enableIntraRefresh = 1;
        ec.encodeCodecConfig.h264Config.intraRefreshPeriod = cfg->refresh_period;
        ec.encodeCodecConfig.h264Config.intraRefreshCnt = cfg->refresh_period < 12u ? cfg->refresh_period - 1u : 10u;
    }

    memset(&ip, 0, sizeof ip);
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
    ip.presetGUID = NV_ENC_PRESET_P1_GUID;
    ip.encodeWidth = cfg->width;
    ip.encodeHeight = cfg->height;
    ip.darWidth = cfg->width;
    ip.darHeight = cfg->height;
    ip.frameRateNum = cfg->fps_num;
    ip.frameRateDen = cfg->fps_den;
    ip.enablePTD = 1;
    ip.encodeConfig = &ec;
    ip.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    ip.maxEncodeWidth = cfg->width;
    ip.maxEncodeHeight = cfg->height;
    st = e->fl.nvEncInitializeEncoder(e->enc, &ip);
    if (st != NV_ENC_SUCCESS) { *why = "NVENC refused this configuration (H.264 4:4:4 at this size)"; goto fail; }

    memset(&ib, 0, sizeof ib);
    ib.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    ib.width = cfg->width;
    ib.height = cfg->height;
    ib.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    if (e->fl.nvEncCreateInputBuffer(e->enc, &ib) != NV_ENC_SUCCESS) { *why = "cannot create the input buffer"; goto fail; }
    e->in = ib.inputBuffer;
    memset(&bb, 0, sizeof bb);
    bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (e->fl.nvEncCreateBitstreamBuffer(e->enc, &bb) != NV_ENC_SUCCESS) { *why = "cannot create the bitstream buffer"; goto fail; }
    e->bs = bb.bitstreamBuffer;
    e->out_cap = (size_t)cfg->width * cfg->height * 3u + 65536u;
    e->out = (uint8_t *)malloc(e->out_cap);
    if (!e->out) { *why = "out of memory"; goto fail; }
    *why = NULL;
    return e;
fail:
    groovy_nvenc_close(e);
    return NULL;
}

int groovy_nvenc_encode(groovy_nvenc *e, const uint8_t *bgr888, int force_key,
                        const uint8_t **out, size_t *len, int *is_key, double *us)
{
    NV_ENC_LOCK_INPUT_BUFFER li;
    NV_ENC_PIC_PARAMS pp;
    NV_ENC_LOCK_BITSTREAM lb;
    LARGE_INTEGER t0, t1;
    unsigned x, y;

    if (!e || !bgr888 || !out || !len) return -1;
    QueryPerformanceCounter(&t0);

    memset(&li, 0, sizeof li);
    li.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    li.inputBuffer = e->in;
    if (e->fl.nvEncLockInputBuffer(e->enc, &li) != NV_ENC_SUCCESS) return -1;
    for (y = 0; y < e->h; y++) {
        const uint8_t *s = bgr888 + (size_t)y * e->w * 3u;
        uint8_t *d = (uint8_t *)li.bufferDataPtr + (size_t)y * li.pitch;
        for (x = 0; x < e->w; x++, s += 3, d += 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0xFF; }
    }
    e->fl.nvEncUnlockInputBuffer(e->enc, e->in);

    memset(&pp, 0, sizeof pp);
    pp.version = NV_ENC_PIC_PARAMS_VER;
    pp.inputWidth = e->w;
    pp.inputHeight = e->h;
    pp.inputPitch = li.pitch;
    pp.inputBuffer = e->in;
    pp.outputBitstream = e->bs;
    pp.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pp.inputTimeStamp = e->frame_idx;
    pp.frameIdx = e->frame_idx;
    if (force_key || e->frame_idx == 0u) pp.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    if (e->fl.nvEncEncodePicture(e->enc, &pp) != NV_ENC_SUCCESS) return -1;

    memset(&lb, 0, sizeof lb);
    lb.version = NV_ENC_LOCK_BITSTREAM_VER;
    lb.outputBitstream = e->bs;
    if (e->fl.nvEncLockBitstream(e->enc, &lb) != NV_ENC_SUCCESS) return -1;
    if ((size_t)lb.bitstreamSizeInBytes > e->out_cap) { e->fl.nvEncUnlockBitstream(e->enc, e->bs); return -1; }
    memcpy(e->out, lb.bitstreamBufferPtr, lb.bitstreamSizeInBytes);
    *len = lb.bitstreamSizeInBytes;
    if (is_key) *is_key = (lb.pictureType == NV_ENC_PIC_TYPE_IDR);
    e->fl.nvEncUnlockBitstream(e->enc, e->bs);
    *out = e->out;
    e->frame_idx++;

    QueryPerformanceCounter(&t1);
    if (us) *us = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)e->qpf.QuadPart;
    return 0;
}

#endif /* _WIN32 */
