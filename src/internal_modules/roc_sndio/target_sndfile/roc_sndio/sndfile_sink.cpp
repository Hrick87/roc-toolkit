/*
 * Copyright (c) 2023 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#define BUFFER_SIZE 512

#include "roc_sndio/sndfile_sink.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#include "roc_sndio/backend_map.h"
#include "roc_sndio/sndfile_extension_table.h"

namespace roc {
namespace sndio {
namespace {

bool map_to_sub_format(SF_INFO& file_info_, int format_enum) {
    // Provides the minimum number of sub formats needed to support all possible major
    // formats
    int high_to_low_sub_formats[4] = { SF_FORMAT_PCM_24, SF_FORMAT_PCM_16,
                                       SF_FORMAT_DPCM_16 };

    for (size_t format_attempt = 0;
         format_attempt < ROC_ARRAY_SIZE(high_to_low_sub_formats); format_attempt++) {
        file_info_.format = format_enum | high_to_low_sub_formats[format_attempt];

        if (sf_format_check(&file_info_)) {
            return true;
        }
    }

    return false;
}

bool map_to_sndfile(const char** driver, const char* path, SF_INFO& file_info_) {
    const char* file_extension;
    const char* dot = strrchr(path, '.');

    if (!dot || dot == path) {
        return false;
    }

    file_extension = dot + 1;

    int format_enum = 0;

    if (*driver == NULL) {
        for (size_t file_map_index = 0; file_map_index < ROC_ARRAY_SIZE(file_type_map);
             file_map_index++) {
            if (file_type_map[file_map_index].file_extension != NULL) {
                if (strcmp(file_extension, file_type_map[file_map_index].file_extension)
                    == 0) {
                    format_enum = file_type_map[file_map_index].format_id;
                    *driver = file_extension;
                    break;
                }
            }
        }
    } else {
        for (size_t file_map_index = 0; file_map_index < ROC_ARRAY_SIZE(file_type_map);
             file_map_index++) {
            if (strcmp(*driver, file_type_map[file_map_index].driver_name) == 0) {
                format_enum = file_type_map[file_map_index].format_id;
                break;
            }
        }
    }

    if (format_enum == 0) {
        SF_FORMAT_INFO info;
        int major_count, format_index;
        if (int errnum =
                sf_command(NULL, SFC_GET_FORMAT_MAJOR_COUNT, &major_count, sizeof(int))) {
            roc_panic("sndfile backend: sf_command(SFC_GET_FORMAT_MAJOR_COUNT) failed %s",
                      sf_error_number(errnum));
        }

        for (format_index = 0; format_index < major_count; format_index++) {
            info.format = format_index;
            if (int errnum =
                    sf_command(NULL, SFC_GET_FORMAT_MAJOR, &info, sizeof(info))) {
                roc_panic("sndfile backend: sf_command(SFC_GET_FORMAT_MAJOR) failed %s",
                          sf_error_number(errnum));
            }

            if (*driver == NULL) {
                if (strcmp(info.extension, file_extension) == 0) {
                    format_enum = info.format;
                    *driver = file_extension;
                    break;
                }
            } else {
                if (strcmp(info.extension, *driver) == 0) {
                    format_enum = info.format;
                    break;
                }
            }
        }
    }

    if (format_enum == 0) {
        return false;
    }

    roc_log(LogDebug, "detected file format type `%s'", *driver);

    file_info_.format = format_enum | file_info_.format;

    if (sf_format_check(&file_info_)) {
        return true;
    } else {
        return map_to_sub_format(file_info_, format_enum);
    }
}

} // namespace

SndfileSink::SndfileSink(core::IArena& arena, const Config& config)
    : file_(NULL)
    , valid_(false) {
    if (config.sample_spec.num_channels() == 0) {
        roc_log(LogError, "sndfile sink: # of channels is zero");
        return;
    }

    if (config.latency != 0) {
        roc_log(LogError,
                "sndfile sink: setting io latency not supported by sndfile backend");
        return;
    }

    frame_length_ = config.frame_length;

    if (frame_length_ == 0) {
        roc_log(LogError, "sndfile sink: frame length is zero");
        return;
    }

    sample_spec_ = config.sample_spec;

    if (sample_spec_.sample_format() == audio::SampleFormat_Invalid) {
        sample_spec_.set_sample_format(audio::SampleFormat_Pcm);
        sample_spec_.set_pcm_format(audio::PcmFormat_SInt32);
    }

    if (sample_spec_.sample_rate() == 0) {
        sample_spec_.set_sample_rate(41000);
    }

    if (!sample_spec_.channel_set().is_valid()) {
        sample_spec_.channel_set().set_layout(audio::ChanLayout_Surround);
        sample_spec_.channel_set().set_order(audio::ChanOrder_Smpte);
        sample_spec_.channel_set().set_channel_range(0, 2, true);
    }

    memset(&file_info_, 0, sizeof(file_info_));

    // TODO(gh-696): map format from sample_space
    file_info_.format = SF_FORMAT_PCM_32;
    file_info_.channels = (int)sample_spec_.num_channels();
    file_info_.samplerate = (int)sample_spec_.sample_rate();

    valid_ = true;
}

SndfileSink::~SndfileSink() {
    close_();
}

bool SndfileSink::is_valid() const {
    return valid_;
}

bool SndfileSink::open(const char* driver, const char* path) {
    roc_panic_if(!valid_);

    roc_log(LogDebug, "sndfile sink: opening: driver=%s path=%s", driver, path);

    if (file_) {
        roc_panic("sndfile sink: can't call open() more than once");
    }

    if (!open_(driver, path)) {
        return false;
    }

    return true;
}

ISink* SndfileSink::to_sink() {
    return this;
}

ISource* SndfileSink::to_source() {
    return NULL;
}

DeviceType SndfileSink::type() const {
    return DeviceType_Sink;
}

DeviceState SndfileSink::state() const {
    return DeviceState_Active;
}

void SndfileSink::pause() {
    // no-op
}

bool SndfileSink::resume() {
    // no-op
    return true;
}

bool SndfileSink::restart() {
    // no-op
    return true;
}

audio::SampleSpec SndfileSink::sample_spec() const {
    roc_panic_if(!valid_);

    if (!file_) {
        roc_panic("sndfile sink: sample_rate(): non-open output file");
    }

    if (file_info_.channels == 1) {
        return audio::SampleSpec(size_t(file_info_.samplerate), audio::Sample_RawFormat,
                                 audio::ChanLayout_Surround, audio::ChanOrder_Smpte,
                                 audio::ChanMask_Surround_Mono);
    }

    if (file_info_.channels == 2) {
        return audio::SampleSpec(size_t(file_info_.samplerate), audio::Sample_RawFormat,
                                 audio::ChanLayout_Surround, audio::ChanOrder_Smpte,
                                 audio::ChanMask_Surround_Stereo);
    }

    roc_panic("sndfile sink: unsupported channel count");
}

core::nanoseconds_t SndfileSink::latency() const {
    roc_panic_if(!valid_);

    if (!file_) {
        roc_panic("sndfile sink: latency(): non-open output file");
    }

    return 0;
}

bool SndfileSink::has_latency() const {
    roc_panic_if(!valid_);

    if (!file_) {
        roc_panic("sndfile sink: has_latency(): non-open output file");
    }

    return false;
}

bool SndfileSink::has_clock() const {
    roc_panic_if(!valid_);

    if (!file_) {
        roc_panic("sndfile sink: has_clock(): non-open output file");
    }

    return false;
}

void SndfileSink::write(audio::Frame& frame) {
    roc_panic_if(!valid_);

    audio::sample_t* frame_data = frame.raw_samples();
    sf_count_t frame_left = (sf_count_t)frame.num_raw_samples();

    // Write entire float buffer in one call
    sf_count_t count = sf_write_float(file_, frame_data, frame_left);

    int errnum = sf_error(file_);
    if (count != frame_left || errnum != 0) {
        // TODO(gh-183): return error instead of panic
        roc_panic("sndfile source: sf_write_float() failed: %s", sf_error_number(errnum));
    }
}

bool SndfileSink::open_(const char* driver, const char* path) {
    unsigned long in_rate = (unsigned long)file_info_.samplerate;

    unsigned long out_rate = (unsigned long)file_info_.samplerate;

    if (!map_to_sndfile(&driver, path, file_info_)) {
        roc_log(LogDebug,
                "sndfile sink: map_to_sndfile(): Cannot find valid subtype format for "
                "major format type");
        return false;
    }

    file_ = sf_open(path, SFM_WRITE, &file_info_);
    if (!file_) {
        roc_log(LogDebug, "sndfile sink: %s, can't open: driver=%s path=%s",
                sf_strerror(file_), driver, path);
        return false;
    }

    if (sf_command(file_, SFC_SET_UPDATE_HEADER_AUTO, NULL, SF_TRUE) == 0) {
        roc_log(LogDebug,
                "sndfile sink: sf_command(SFC_SET_UPDATE_HEADER_AUTO) returned false");
        return false;
    }

    sample_spec_.set_sample_rate((unsigned long)file_info_.samplerate);

    roc_log(LogInfo,
            "sndfile sink:"
            " opened: out_rate=%lu in_rate=%lu ch=%lu",
            out_rate, in_rate, (unsigned long)file_info_.channels);

    sf_count_t err = sf_seek(file_, 0, SEEK_SET);
    if (err == -1) {
        roc_log(LogError, "sndfile sink: sf_seek(): %s", sf_strerror(file_));
        return false;
    }

    return true;
}

void SndfileSink::close_() {
    if (!file_) {
        return;
    }

    roc_log(LogDebug, "sndfile sink: closing output");

    int err = sf_close(file_);
    if (err != 0) {
        roc_panic("sndfile sink: sf_close() failed. Cannot close output: %s",
                  sf_error_number(err));
    }

    file_ = NULL;
}

} // namespace sndio
} // namespace roc
