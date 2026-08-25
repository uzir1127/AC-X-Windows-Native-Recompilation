#include "tex_ktx2.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../third_party/basis_universal/transcoder/basisu_transcoder.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdlib.h>
#include <string.h>

static const uint8_t k_ktx2_id[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
};

extern "C" int ktx2_is_ktx2(const uint8_t *data, size_t len)
{
    return data && len >= sizeof k_ktx2_id && memcmp(data, k_ktx2_id, sizeof k_ktx2_id) == 0;
}

static void init_once()
{
    static bool done = false;
    if (!done) {
        basist::basisu_transcoder_init();
        done = true;
    }
}

extern "C" int ktx2_decode_levels(const uint8_t *data, size_t len, int max_levels,
                                  uint32_t **px, int *w, int *h, const char **err)
{
    if (err) *err = NULL;
    if (!ktx2_is_ktx2(data, len) || max_levels <= 0) {
        if (err) *err = "not a KTX2 file";
        return 0;
    }

    init_once();

    basist::ktx2_transcoder tr;
    if (!tr.init(data, (uint32_t)len)) {
        if (err) *err = "KTX2 header rejected";
        return 0;
    }
    if (tr.is_hdr()) {
        if (err) *err = "HDR KTX2 is not supported";
        return 0;
    }
    if (tr.get_faces() > 1 || tr.get_layers() > 1) {
        if (err) *err = "cubemaps and array textures are not supported";
        return 0;
    }
    if (!tr.start_transcoding()) {
        if (err) *err = "KTX2 supercompression could not be decoded";
        return 0;
    }

    int levels = (int)tr.get_levels();
    if (levels < 1) levels = 1;
    if (levels > max_levels) levels = max_levels;

    int done = 0;
    for (int i = 0; i < levels; i++) {
        basist::ktx2_image_level_info li;
        memset(&li, 0, sizeof li);
        if (!tr.get_image_level_info(li, (uint32_t)i, 0, 0)) break;
        if (li.m_orig_width == 0 || li.m_orig_height == 0) break;

        size_t n = (size_t)li.m_orig_width * (size_t)li.m_orig_height;
        uint32_t *buf = (uint32_t *)malloc(n * 4);
        if (!buf) break;

        if (!tr.transcode_image_level((uint32_t)i, 0, 0, buf, (uint32_t)n,
                                      basist::transcoder_texture_format::cTFRGBA32,
                                      0, li.m_orig_width, li.m_orig_height, -1, -1, NULL)) {
            free(buf);
            break;
        }
        px[i] = buf;
        w[i] = (int)li.m_orig_width;
        h[i] = (int)li.m_orig_height;
        done = i + 1;
    }

    tr.clear();

    if (!done && err) *err = "KTX2 transcode failed";
    return done;
}
