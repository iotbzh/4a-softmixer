/*
 * Copyright (C) 2018 "IoT.bzh"
 * Author Fulup Ar Foll <fulup@iot.bzh>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#define _GNU_SOURCE  // needed for vasprintf

#include "alsa-softmixer.h"
#include <math.h>

// move from vol % to absolute value
#define CONVERT_RANGE(val, min, max) ceil((val) * ((max) - (min)) * 0.01 + (min))
#define CONVERT_VOLUME(val, min, max) (int) CONVERT_RANGE ((double)val, (double)min, (double)max)


STATIC AlsaPcmChannelT *ProcessOneChannel(SoftMixerT *mixer, const char *uid, json_object *argsJ) {
    AlsaPcmChannelT *channel = calloc(1, sizeof (AlsaPcmChannelT));
    int error = wrap_json_unpack(argsJ, "{ss,si !}", "uid", &channel->uid, "port", &channel->port);
    if (error) goto OnErrorExit;

    channel->uid = strdup(channel->uid);
    return channel;

OnErrorExit:
    AFB_ApiError(mixer->api, "ProcessOneChannel: sndcard=%s channel: missing (uid||port) error=%s json=%s", uid, wrap_json_get_error_string(error), json_object_get_string(argsJ));
    free(channel);
    return NULL;
}

STATIC int ProcessOneControl(SoftMixerT *mixer, AlsaSndPcmT* pcm, json_object *argsJ, AlsaSndControlT *control) {
    snd_ctl_elem_id_t* elemId = NULL;
    snd_ctl_elem_info_t *elemInfo;
    int numid = 0;
    long value = ALSA_DEFAULT_PCM_VOLUME;
    const char *name;


    int error = wrap_json_unpack(argsJ, "{s?i,s?s,s?i !}"
            , "numid", &numid
            , "name", &name
            , "value", &value
            );
    if (error || (!numid && !name)) {
        AFB_ApiError(mixer->api, "ProcessOneControl: sndcard=%s channel: missing (numid|name|value) error=%s json=%s", pcm->uid, wrap_json_get_error_string(error), json_object_get_string(argsJ));
        goto OnErrorExit;
    }

    if (numid > 0) {
        elemId = AlsaCtlGetNumidElemId(mixer, pcm->sndcard, numid);
        if (!elemId) {
            AFB_ApiError(mixer->api, "ProcessOneControl sndard=%s fail to find control numid=%d", pcm->sndcard->cid.cardid, numid);
            goto OnErrorExit;
        }

    } else {
        elemId = AlsaCtlGetNameElemId(mixer, pcm->sndcard, name);
        if (!elemId) {
            AFB_ApiError(mixer->api, "ProcessOneControl sndard=%s fail to find control name=%s", pcm->sndcard->cid.cardid, name);
            goto OnErrorExit;
        }
    }

    snd_ctl_elem_info_alloca(&elemInfo);
    snd_ctl_elem_info_set_id(elemInfo, elemId);
    control->name = strdup(snd_ctl_elem_info_get_name(elemInfo));
    control->numid = snd_ctl_elem_info_get_numid(elemInfo);

    if (snd_ctl_elem_info(pcm->sndcard->ctl, elemInfo) < 0) {
        AFB_ApiError(mixer->api, "ProcessOneControl: sndard=%s numid=%d name='%s' not loadable", pcm->sndcard->cid.cardid, control->numid, control->name);
        goto OnErrorExit;
    }

    if (!snd_ctl_elem_info_is_writable(elemInfo)) {
        AFB_ApiError(mixer->api, "ProcessOneControl: sndard=%s numid=%d name='%s' not writable", pcm->sndcard->cid.cardid, control->numid, control->name);
        goto OnErrorExit;
    }

    control->count = snd_ctl_elem_info_get_count(elemInfo);
    switch (snd_ctl_elem_info_get_type(elemInfo)) {
        case SND_CTL_ELEM_TYPE_BOOLEAN:
            control->min = 0;
            control->max = 1;
            control->step = 0;
            error = CtlElemIdSetLong(mixer, pcm->sndcard, elemId, value);
            break;

        case SND_CTL_ELEM_TYPE_INTEGER:
        case SND_CTL_ELEM_TYPE_INTEGER64:
            control->min = snd_ctl_elem_info_get_min(elemInfo);
            control->max = snd_ctl_elem_info_get_max(elemInfo);
            control->step = snd_ctl_elem_info_get_step(elemInfo);
            error = CtlElemIdSetLong(mixer, pcm->sndcard, elemId, (int) CONVERT_VOLUME(value, control->min, control->max));
            break;

        default:
            AFB_ApiError(mixer->api, "ProcessOneControl: sndard=%s numid=%d name='%s' invalid/unsupported type=%d", pcm->sndcard->cid.cardid, control->numid, control->name, snd_ctl_elem_info_get_type(elemInfo));
            goto OnErrorExit;
    }

    if (error) {
        AFB_ApiError(mixer->api, "ProcessOneControl: sndard=%s numid=%d name='%s' not writable", pcm->sndcard->cid.cardid, control->numid, control->name);
        goto OnErrorExit;
    }

    free(elemId);

    return 0;

OnErrorExit:
    if (elemId)free(elemId);
    return -1;
}

PUBLIC AlsaPcmHwInfoT *ApiPcmSetParams(SoftMixerT *mixer, const char *uid, json_object *paramsJ) {
    AlsaPcmHwInfoT *params = calloc(1, sizeof (AlsaPcmHwInfoT));
    const char *format = NULL, *access = NULL;

    // some default values
    params->rate = ALSA_DEFAULT_PCM_RATE;
    params->channels = 2;
    params->sampleSize = 0;

    if (paramsJ) {
        int error = wrap_json_unpack(paramsJ, "{s?i,s?i, s?s, s?s !}", "rate", &params->rate, "channels", &params->channels, "format", &format, "access", &access);
        if (error) {
            AFB_ApiError(mixer->api, "ApiPcmSetParams: sndcard=%s invalid params=%s", uid, json_object_get_string(paramsJ));
            goto OnErrorExit;
        }
    }

    if (!format) {
        params->format = SND_PCM_FORMAT_S16_LE;
        params->formatS = "S16_LE";
    } else {
        params->formatS = strdup(format);
        if (!strcasecmp(format, "S16_LE")) params->format = SND_PCM_FORMAT_S16_LE;
        else if (!strcasecmp(format, "S16_BE")) params->format = SND_PCM_FORMAT_S16_BE;
        else if (!strcasecmp(format, "U16_LE")) params->format = SND_PCM_FORMAT_U16_LE;
        else if (!strcasecmp(format, "U16_BE")) params->format = SND_PCM_FORMAT_U16_BE;
        else if (!strcasecmp(format, "S32_LE")) params->format = SND_PCM_FORMAT_S32_LE;
        else if (!strcasecmp(format, "S32_BE")) params->format = SND_PCM_FORMAT_S32_BE;
        else if (!strcasecmp(format, "U32_LE")) params->format = SND_PCM_FORMAT_U32_LE;
        else if (!strcasecmp(format, "U32_BE")) params->format = SND_PCM_FORMAT_U32_BE;
        else if (!strcasecmp(format, "S24_LE")) params->format = SND_PCM_FORMAT_S24_LE;
        else if (!strcasecmp(format, "S24_BE")) params->format = SND_PCM_FORMAT_S24_BE;
        else if (!strcasecmp(format, "U24_LE")) params->format = SND_PCM_FORMAT_U24_LE;
        else if (!strcasecmp(format, "U24_BE")) params->format = SND_PCM_FORMAT_U24_BE;
        else if (!strcasecmp(format, "S8")) params->format = SND_PCM_FORMAT_S8;
        else if (!strcasecmp(format, "U8")) params->format = SND_PCM_FORMAT_U8;
        else if (!strcasecmp(format, "FLOAT_LE")) params->format = SND_PCM_FORMAT_FLOAT_LE;
        else if (!strcasecmp(format, "FLOAT_BE")) params->format = SND_PCM_FORMAT_FLOAT_LE;
        else {
            AFB_ApiNotice(mixer->api, "ApiPcmSetParams:%s(params) unsupported format 'S16_LE|S32_L|...' format=%s", uid, format);
            goto OnErrorExit;
        }
    }


    if (!access) params->access = SND_PCM_ACCESS_RW_INTERLEAVED;
    else if (!strcasecmp(access, "MMAP_INTERLEAVED")) params->access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
    else if (!strcasecmp(access, "MMAP_NONINTERLEAVED")) params->access = SND_PCM_ACCESS_MMAP_NONINTERLEAVED;
    else if (!strcasecmp(access, "MMAP_COMPLEX")) params->access = SND_PCM_ACCESS_MMAP_COMPLEX;
    else if (!strcasecmp(access, "RW_INTERLEAVED")) params->access = SND_PCM_ACCESS_RW_INTERLEAVED;
    else if (!strcasecmp(access, "RW_NONINTERLEAVED")) params->access = SND_PCM_ACCESS_RW_NONINTERLEAVED;

    else {
        AFB_ApiNotice(mixer->api, "ApiPcmSetParams:%s(params) unsupported access 'RW_INTERLEAVED|MMAP_INTERLEAVED|MMAP_COMPLEX' access=%s", uid, access);
        goto OnErrorExit;
    }

    return params;

OnErrorExit:
    free(params);
    return NULL;
}

PUBLIC AlsaSndPcmT *ApiPcmAttachOne(SoftMixerT *mixer, const char *uid, snd_pcm_stream_t direction, json_object *argsJ) {
    AlsaSndPcmT *pcm = calloc(1, sizeof (AlsaSndPcmT));
    json_object *sourceJ = NULL, *paramsJ = NULL, *sinkJ = NULL, *targetJ;
    int error;

    pcm->sndcard = (AlsaSndCtlT*) calloc(1, sizeof (AlsaSndCtlT));
    error = wrap_json_unpack(argsJ, "{ss,s?s,s?s,s?i,s?i,s?o,s?o,s?o !}"
            , "uid", &pcm->uid
            , "path", &pcm->sndcard->cid.devpath
            , "cardid", &pcm->sndcard->cid.cardid
            , "device", &pcm->sndcard->cid.device
            , "subdev", &pcm->sndcard->cid.subdev
            , "sink", &sinkJ
            , "source", &sourceJ
            , "params", &paramsJ
            );
    if (error) {
        AFB_ApiError(mixer->api, "ApiPcmAttachOne: hal=%s missing 'uid|path|cardid|device|sink|source|params' error=%s args=%s", uid, wrap_json_get_error_string(error), json_object_get_string(argsJ));
        goto OnErrorExit;
    }

    // try to open sound card control interface
    pcm->sndcard->ctl = AlsaByPathOpenCtl(mixer, pcm->uid, pcm->sndcard);
    if (!pcm->sndcard->ctl) {
        AFB_ApiError(mixer->api, "ApiPcmAttachOne: hal=%s Fail to open sndcard uid=%s devpath=%s cardid=%s", uid, pcm->uid, pcm->sndcard->cid.devpath, pcm->sndcard->cid.cardid);
        goto OnErrorExit;
    }
    
    // check sndcard accepts params
    pcm->sndcard->params = ApiPcmSetParams(mixer, pcm->uid, paramsJ);
    if (!pcm->sndcard->params) {
        AFB_ApiError(mixer->api, "ApiPcmAttachOne: hal=%s  Fail to set params sndcard uid=%s params=%s", uid, pcm->uid, json_object_get_string(paramsJ));
        goto OnErrorExit;
    }

    if (direction == SND_PCM_STREAM_PLAYBACK) {
        if (!sinkJ) {
            AFB_ApiError(mixer->api, "ApiPcmAttachOne: hal=%s SND_PCM_STREAM_PLAYBACK require sinks args=%s", uid, json_object_get_string(argsJ));
            goto OnErrorExit;
        }
        targetJ = sinkJ;
    }

    if (direction == SND_PCM_STREAM_CAPTURE) {
        if (!sourceJ) {
            AFB_ApiError(mixer->api, "ApiPcmAttachOne: hal=%s SND_PCM_STREAM_CAPTURE require sources args=%s", uid, json_object_get_string(argsJ));
            goto OnErrorExit;
        }
        targetJ = sourceJ;

        // we may have to register SMIXER_SUBDS_CTLS per subdev (Fulup ToBeDone when sndcard get multiple device/subdev)
        pcm->sndcard->registry = calloc(SMIXER_SUBDS_CTLS+1, sizeof (RegistryEntryPcmT));
        pcm->sndcard->rcount = SMIXER_SUBDS_CTLS;
    }

    json_object *channelsJ = NULL, *controlsJ = NULL;
    error = wrap_json_unpack(targetJ, "{so,s?o !}"
            , "channels", &channelsJ
            , "controls", &controlsJ
            );
    if (error) {
        AFB_ApiNotice(mixer->api, "ApiPcmAttachOne: hal=%s pcms missing channels|[controls] error=%s paybacks=%s", uid, wrap_json_get_error_string(error), json_object_get_string(argsJ));
        goto OnErrorExit;
    }
    if (channelsJ) {
        switch (json_object_get_type(channelsJ)) {

            case json_type_object:
                pcm->ccount = 1;
                pcm->channels = calloc(2, sizeof (void*));
                pcm->channels[0] = ProcessOneChannel(mixer, pcm->uid, channelsJ);
                if (!pcm->channels[0]) goto OnErrorExit;
                break;
            case json_type_array:
                pcm->ccount = (int) json_object_array_length(channelsJ);
                pcm->channels = calloc(pcm->ccount + 1, sizeof (void*));
                for (int idx = 0; idx < pcm->ccount; idx++) {
                    json_object *channelJ = json_object_array_get_idx(channelsJ, idx);
                    pcm->channels[idx] = ProcessOneChannel(mixer, pcm->uid, channelJ);
                    if (!pcm->channels[idx]) goto OnErrorExit;
                }
                break;
            default:
                AFB_ApiError(mixer->api, "ProcessPcmControls:%s invalid pcm=%s", pcm->uid, json_object_get_string(channelsJ));
                goto OnErrorExit;
        }
    }

    if (controlsJ) {
        json_object *volJ = NULL, *muteJ = NULL;
        error = wrap_json_unpack(controlsJ, "{s?o,s?o !}"
                , "volume", &volJ
                , "mute", &muteJ
                );
        if (error) {
            AFB_ApiNotice(mixer->api, "ProcessPcmControls: source missing [volume]|[mute] error=%s control=%s", wrap_json_get_error_string(error), json_object_get_string(controlsJ));
            goto OnErrorExit;
        }

        if (volJ) error += ProcessOneControl(mixer, pcm, volJ, &pcm->volume);
        if (muteJ) error += ProcessOneControl(mixer, pcm, muteJ, &pcm->mute);
        if (error) goto OnErrorExit;
    }

    // free useless resource and secure others
    pcm->uid = strdup(pcm->uid);

    return pcm;

OnErrorExit:
    free(pcm);
    return NULL;
}
