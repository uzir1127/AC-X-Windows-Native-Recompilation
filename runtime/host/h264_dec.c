#include "h264_dec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>

DEFINE_GUID(k_CLSID_CMSH264DecoderMFT,
            0x62CE7E72, 0x4C71, 0x4d20,
            0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D);
DEFINE_GUID(k_MF_LOW_LATENCY,
            0x9c27891a, 0xed7a, 0x40e1,
            0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee);

struct H264Dec {
    IMFTransform *xf;
    int failed;
    int out_w, out_h;
    int out_stride;
    int drained;

    IMFSample *out_sample;
    IMFMediaBuffer *out_buf;
    DWORD out_buf_size;

    uint8_t *es;
    uint32_t es_len, es_cap, es_pos;
    int saw_aud;
    LONGLONG fake_ts;
};

enum { MF_UNTRIED = 0, MF_UP = 1, MF_FAILED = -1 };
static int g_mf_state;

#define NAL_TYPE_MASK             0x1F
#define NAL_TYPE_SLICE_NON_IDR    1
#define NAL_TYPE_SLICE_IDR        5
#define NAL_TYPE_AUD              9
#define FIRST_MB_IN_SLICE_ZERO_BIT 0x80

#define ES_COMPACT_THRESHOLD      (1u << 20)
#define AU_WAIT_MAX_PENDING       (1u << 20)
#define FRAME_DURATION_100NS      333667

static int mf_up(void)
{
    if (g_mf_state != MF_UNTRIED) return g_mf_state == MF_UP;
    g_mf_state = MF_FAILED;
    (void)CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        fprintf(stderr, "[h264] MFStartup failed; movies will be blank\n");
        return 0;
    }
    g_mf_state = MF_UP;
    return 1;
}

static int set_output_type(H264Dec *d);

static int set_types(H264Dec *d)
{
    IMFMediaType *t = NULL;
    HRESULT hr;

    if (FAILED(MFCreateMediaType(&t))) return 0;
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    hr = IMFTransform_SetInputType(d->xf, 0, t, 0);
    IMFMediaType_Release(t);
    if (FAILED(hr)) return 0;
    return set_output_type(d);
}

static int set_output_type(H264Dec *d)
{
    IMFMediaType *t = NULL;
    int ok = 0;

    for (DWORD i = 0; ; i++) {
        GUID sub;
        if (FAILED(IMFTransform_GetOutputAvailableType(d->xf, 0, i, &t)))
            break;
        if (SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub))
            && IsEqualGUID(&sub, &MFVideoFormat_NV12)) {
            UINT64 fs = 0;
            UINT32 stride = 0;
            if (SUCCEEDED(IMFTransform_SetOutputType(d->xf, 0, t, 0))) {
                if (SUCCEEDED(IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &fs))) {
                    d->out_w = (int)(fs >> 32);
                    d->out_h = (int)(fs & 0xFFFFFFFFu);
                }
                if (SUCCEEDED(IMFMediaType_GetUINT32(t, &MF_MT_DEFAULT_STRIDE,
                                                     &stride)) && stride)
                    d->out_stride = (int)stride;
                else
                    d->out_stride = d->out_w;
                ok = 1;
            }
            IMFMediaType_Release(t);
            break;
        }
        IMFMediaType_Release(t);
    }
    return ok;
}

H264Dec *h264_create(void)
{
    H264Dec *d;
    IMFAttributes *attr = NULL;

    if (getenv("PSPRECO_NOH264")) return NULL;
    if (!mf_up()) return NULL;
    d = (H264Dec *)calloc(1, sizeof *d);
    if (!d) return NULL;
    if (FAILED(CoCreateInstance(&k_CLSID_CMSH264DecoderMFT, NULL,
                                CLSCTX_INPROC_SERVER, &IID_IMFTransform,
                                (void **)&d->xf))) {
        fprintf(stderr, "[h264] H.264 decoder MFT unavailable\n");
        free(d);
        return NULL;
    }
    if (SUCCEEDED(IMFTransform_GetAttributes(d->xf, &attr))) {
        IMFAttributes_SetUINT32(attr, &k_MF_LOW_LATENCY, 1);
        IMFAttributes_Release(attr);
    }
    if (!set_types(d)) {
        fprintf(stderr, "[h264] type negotiation failed\n");
        h264_destroy(d);
        return NULL;
    }
    IMFTransform_ProcessMessage(d->xf, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(d->xf, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return d;
}

void h264_destroy(H264Dec *d)
{
    if (!d) return;
    if (d->out_sample) IMFSample_Release(d->out_sample);
    if (d->out_buf) IMFMediaBuffer_Release(d->out_buf);
    if (d->xf) IMFTransform_Release(d->xf);
    free(d->es);
    free(d);
}

void h264_feed(H264Dec *d, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    if (!d || d->failed || !data || !len) return;
    if (!d->saw_aud)
        for (i = 0; i + 4 < len; i++)
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1
                && (data[i + 3] & 0x1F) == 9) {
                d->saw_aud = 1;
                break;
            }
    if (d->es_len + len > d->es_cap) {
        uint32_t n = d->es_cap ? d->es_cap : (1u << 16);
        while (n < d->es_len + len) n *= 2;
        uint8_t *p = (uint8_t *)realloc(d->es, n);
        if (!p) return;
        d->es = p;
        d->es_cap = n;
    }
    memcpy(d->es + d->es_len, data, len);
    d->es_len += len;
}

static uint32_t next_sc(const H264Dec *d, uint32_t from, int *sclen)
{
    const uint8_t *p = d->es;
    uint32_t i = from;
    while (i + 3 <= d->es_len) {
        if (p[i] == 0 && p[i + 1] == 0) {
            if (p[i + 2] == 1) { *sclen = 3; return i; }
            if (i + 4 <= d->es_len && p[i + 2] == 0 && p[i + 3] == 1) {
                *sclen = 4;
                return i;
            }
        }
        i++;
    }
    *sclen = 0;
    return d->es_len;
}

static int is_new_picture(const H264Dec *d, uint32_t pos, int sclen)
{
    uint32_t h = pos + (uint32_t)sclen;
    int type;
    if (h + 1 >= d->es_len) return 0;
    type = d->es[h] & NAL_TYPE_MASK;
    if (type == NAL_TYPE_AUD) return 1;
    if (d->saw_aud) return 0;
    if (type != NAL_TYPE_SLICE_NON_IDR && type != NAL_TYPE_SLICE_IDR) return 0;
    return (d->es[h + 1] & FIRST_MB_IN_SLICE_ZERO_BIT) != 0;
}

static int next_au(H264Dec *d, int eos, uint32_t *start, uint32_t *end)
{
    uint32_t pos = d->es_pos, first_pic = 0, scan;
    int sclen, have_pic = 0;

    scan = next_sc(d, pos, &sclen);
    if (scan >= d->es_len) return 0;
    pos = scan;

    for (scan = pos; scan < d->es_len; ) {
        if (is_new_picture(d, scan, sclen)) { first_pic = scan; have_pic = 1; break; }
        scan = next_sc(d, scan + 3, &sclen);
    }
    if (!have_pic) {
        if (eos && d->es_len > d->es_pos) {
            *start = d->es_pos;
            *end = d->es_len;
            d->es_pos = d->es_len;
            return 1;
        }
        return 0;
    }

    scan = next_sc(d, first_pic + 3, &sclen);
    while (scan < d->es_len && !is_new_picture(d, scan, sclen))
        scan = next_sc(d, scan + 3, &sclen);
    if (scan >= d->es_len && !eos) {
        if (d->es_len - d->es_pos < AU_WAIT_MAX_PENDING) return 0;
    }
    *start = d->es_pos;
    *end = (scan < d->es_len) ? scan : d->es_len;
    d->es_pos = *end;
    return *end > *start;
}

static void es_compact(H264Dec *d)
{
    if (d->es_pos < ES_COMPACT_THRESHOLD) return;
    memmove(d->es, d->es + d->es_pos, d->es_len - d->es_pos);
    d->es_len -= d->es_pos;
    d->es_pos = 0;
}

static uint8_t clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static void convert_nv12(const uint8_t *y_base, int y_stride,
                         const uint8_t *uv_base, int uv_stride,
                         int w, int h, uint8_t *dst, int frameWidth,
                         int pixelMode)
{
    int x, y;
    if (w > frameWidth) w = frameWidth;
    if (h > 272) h = 272;
    for (y = 0; y < h; y++) {
        const uint8_t *yr = y_base + (size_t)y * y_stride;
        const uint8_t *uv = uv_base + (size_t)(y / 2) * uv_stride;
        uint8_t *o8 = dst + (size_t)y * frameWidth * 4;
        uint16_t *o16 = (uint16_t *)(dst + (size_t)y * frameWidth * 2);
        for (x = 0; x < w; x++) {
            int ci = (x >> 1) * 2;
            int Y = (yr[x] - 16) * 298;
            int U = uv[ci] - 128, V = uv[ci + 1] - 128;
            int r = clamp8((Y + 409 * V + 128) >> 8);
            int g = clamp8((Y - 100 * U - 208 * V + 128) >> 8);
            int b = clamp8((Y + 516 * U + 128) >> 8);
            switch (pixelMode) {
            case 0:
                o16[x] = (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
                break;
            case 1:
                o16[x] = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
                                    | 0x8000);
                break;
            case 2:
                o16[x] = (uint16_t)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8)
                                    | 0xF000);
                break;
            default:
                o8[x * 4 + 0] = (uint8_t)r;
                o8[x * 4 + 1] = (uint8_t)g;
                o8[x * 4 + 2] = (uint8_t)b;
                o8[x * 4 + 3] = 0xFF;
                break;
            }
        }
    }
}


static int send_au(H264Dec *d, const uint8_t *au, uint32_t len)
{
    IMFSample *smp = NULL;
    IMFMediaBuffer *mb = NULL;
    BYTE *ptr = NULL;
    HRESULT hr;

    if (FAILED(MFCreateSample(&smp))) return -1;
    if (FAILED(MFCreateMemoryBuffer(len, &mb))) {
        IMFSample_Release(smp);
        return -1;
    }
    if (FAILED(IMFMediaBuffer_Lock(mb, &ptr, NULL, NULL)) || !ptr) {
        IMFMediaBuffer_Release(mb);
        IMFSample_Release(smp);
        return -1;
    }
    memcpy(ptr, au, len);
    IMFMediaBuffer_Unlock(mb);
    IMFMediaBuffer_SetCurrentLength(mb, len);
    IMFSample_AddBuffer(smp, mb);
    IMFSample_SetSampleTime(smp, d->fake_ts);
    IMFSample_SetSampleDuration(smp, FRAME_DURATION_100NS);
    d->fake_ts += FRAME_DURATION_100NS;

    hr = IMFTransform_ProcessInput(d->xf, 0, smp, 0);
    IMFMediaBuffer_Release(mb);
    IMFSample_Release(smp);
    if (hr == MF_E_NOTACCEPTING) return 0;
    if (FAILED(hr))
        fprintf(stderr, "[h264] ProcessInput hr=0x%08lx len=%u\n",
                (unsigned long)hr, (unsigned)len);
    return SUCCEEDED(hr) ? 1 : -1;
}

static int ensure_out_sample(H264Dec *d)
{
    MFT_OUTPUT_STREAM_INFO info;

    memset(&info, 0, sizeof info);
    if (FAILED(IMFTransform_GetOutputStreamInfo(d->xf, 0, &info))) return 0;
    if (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES
                        | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) {
        if (d->out_sample) { IMFSample_Release(d->out_sample); d->out_sample = NULL; }
        if (d->out_buf) { IMFMediaBuffer_Release(d->out_buf); d->out_buf = NULL; }
        return 1;
    }
    if (d->out_sample && d->out_buf_size >= info.cbSize) return 1;
    if (d->out_sample) { IMFSample_Release(d->out_sample); d->out_sample = NULL; }
    if (d->out_buf) { IMFMediaBuffer_Release(d->out_buf); d->out_buf = NULL; }
    if (FAILED(MFCreateSample(&d->out_sample))) return 0;
    if (FAILED(MFCreateMemoryBuffer(info.cbSize ? info.cbSize : (1u << 20),
                                    &d->out_buf))) {
        IMFSample_Release(d->out_sample);
        d->out_sample = NULL;
        return 0;
    }
    IMFSample_AddBuffer(d->out_sample, d->out_buf);
    d->out_buf_size = info.cbSize;
    return 1;
}

static int recv_frame(H264Dec *d, uint8_t *dst, int frameWidth, int pixelMode)
{
    MFT_OUTPUT_DATA_BUFFER out;
    DWORD status = 0;
    HRESULT hr;

    if (!ensure_out_sample(d)) return -1;
    if (d->out_buf) IMFMediaBuffer_SetCurrentLength(d->out_buf, 0);
    memset(&out, 0, sizeof out);
    out.pSample = d->out_sample;
    hr = IMFTransform_ProcessOutput(d->xf, 0, 1, &out, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return 0;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        int ok;
        if (out.pEvents) IMFCollection_Release(out.pEvents);
        ok = set_output_type(d);
        fprintf(stderr, "[h264] stream change -> renegotiate %s (%dx%d stride %d)\n",
                ok ? "ok" : "FAILED", d->out_w, d->out_h, d->out_stride);
        return ok ? 0 : -1;
    }
    if (FAILED(hr) || !out.pSample) {
        fprintf(stderr, "[h264] ProcessOutput hr=0x%08lx sample=%p status=%lu\n",
                (unsigned long)hr, (void *)out.pSample,
                (unsigned long)out.dwStatus);
        if (out.pEvents) IMFCollection_Release(out.pEvents);
        return -1;
    }
    {
        IMFMediaBuffer *mb = NULL;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(out.pSample, &mb))) {
            BYTE *ptr = NULL;
            DWORD len = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &ptr, NULL, &len))) {
                int w = d->out_w ? d->out_w : 480;
                int h = d->out_h ? d->out_h : 272;
                int stride = d->out_stride ? d->out_stride : w;
                if ((DWORD)(stride * h * 3 / 2) <= len && dst)
                    convert_nv12(ptr, stride,
                                 ptr + (size_t)stride * h, stride,
                                 w, h, dst, frameWidth, pixelMode);
                IMFMediaBuffer_Unlock(mb);
            }
            IMFMediaBuffer_Release(mb);
        }
        if (out.pSample != d->out_sample)
            IMFSample_Release(out.pSample);
    }
    if (out.pEvents) IMFCollection_Release(out.pEvents);
    return 1;
}

int h264_frame(H264Dec *d, int eos, uint8_t *dst, int frameWidth,
               int pixelMode)
{
    int guard;

    if (!d || d->failed) return -1;
    for (guard = 0; guard < 4096; guard++) {
        uint32_t s, e;
        int r = recv_frame(d, dst, frameWidth, pixelMode);
        if (r != 0) {
            if (r < 0) {
                d->failed = 1;
                return -1;
            }
            return 1;
        }
        if (next_au(d, eos, &s, &e)) {
            static int au_dbg;
            if (au_dbg < 8)
                fprintf(stderr, "[h264] AU %d: %u bytes\n", au_dbg++, e - s);
            r = send_au(d, d->es + s, e - s);
            if (r < 0) { d->failed = 1; return -1; }
            if (r == 0)
                d->es_pos = s;
            else
                es_compact(d);
            continue;
        }
        if (eos && !d->drained) {
            d->drained = 1;
            IMFTransform_ProcessMessage(d->xf, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            IMFTransform_ProcessMessage(d->xf, MFT_MESSAGE_COMMAND_DRAIN, 0);
            continue;
        }
        return 0;
    }
    return 0;
}

#else

H264Dec *h264_create(void) { return NULL; }
void h264_destroy(H264Dec *d) { (void)d; }
void h264_feed(H264Dec *d, const uint8_t *data, uint32_t len)
{ (void)d; (void)data; (void)len; }
int h264_frame(H264Dec *d, int eos, uint8_t *dst, int frameWidth,
               int pixelMode)
{ (void)d; (void)eos; (void)dst; (void)frameWidth; (void)pixelMode; return -1; }

#endif
