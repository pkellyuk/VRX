#pragma once
// PipeWire video format offers shared by the capture adapter and probe.
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <cstdint>
#include <vector>

namespace vrx {
// One EnumFormat: a BGRA-family format with DMA-BUF `modifiers`, or (null)
// shared memory in BGRA, BGRx or RGBA. Several modifiers are offered for the
// producer to choose from (DONT_FIXATE); a single one is a fixed choice.
inline const spa_pod* BuildCaptureFormat(spa_pod_builder* builder, uint32_t format,
                                         const std::vector<uint64_t>* modifiers) {
    const spa_rectangle default_size = {1920, 1080}, min_size = {1, 1}, max_size = {8192, 8192};
    const spa_fraction default_rate = {60, 1}, min_rate = {0, 1}, max_rate = {240, 1};
    spa_pod_frame object{}, choice{};
    spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(builder, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    if (modifiers) {
        spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
        if (modifiers->size() == 1) {
            spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
            spa_pod_builder_long(builder, int64_t(modifiers->front()));
        } else {
            spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier,
                                 SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
            spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
            spa_pod_builder_long(builder, int64_t(modifiers->front()));
            for (uint64_t modifier : *modifiers) spa_pod_builder_long(builder, int64_t(modifier));
            spa_pod_builder_pop(builder, &choice);
        }
    } else {
        spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(4,
            SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA), 0);
    }
    spa_pod_builder_add(builder,
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&default_size, &min_size, &max_size),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&default_rate, &min_rate, &max_rate), 0);
    return static_cast<const spa_pod*>(spa_pod_builder_pop(builder, &object));
}

// DMA-BUF BGRA and BGRx with `modifiers` (if any), then shared memory as the fallback.
inline std::vector<const spa_pod*> BuildCaptureFormats(spa_pod_builder* builder,
                                                       const std::vector<uint64_t>& modifiers) {
    std::vector<const spa_pod*> formats;
    if (!modifiers.empty()) {
        formats.push_back(BuildCaptureFormat(builder, SPA_VIDEO_FORMAT_BGRA, &modifiers));
        formats.push_back(BuildCaptureFormat(builder, SPA_VIDEO_FORMAT_BGRx, &modifiers));
    }
    formats.push_back(BuildCaptureFormat(builder, 0, nullptr));
    return formats;
}
} // namespace vrx
