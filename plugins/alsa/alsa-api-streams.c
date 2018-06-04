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
#include <string.h>

// Set stream volume control in %
#define VOL_CONTROL_MAX  100
#define VOL_CONTROL_MIN  0
#define VOL_CONTROL_STEP 1

// Fulup need to be cleanup with new controller version
extern Lua2cWrapperT Lua2cWrap;

typedef struct {
    AlsaStreamAudioT *stream;
    SoftMixerT *mixer;
    AlsaVolRampT *ramp;
    AlsaSndCtlT *sndcard;
    snd_pcm_t *pcm;
} apiHandleT;

STATIC void StreamApiVerbCB(AFB_ReqT request) {
    apiHandleT *handle = (apiHandleT*) afb_request_get_vcbdata(request);
    int error, doClose = 0, doQuiet = 0, doToggle = 0, doMute = -1;
    long mute, volume;
    json_object *responseJ, *volumeJ = NULL, *rampJ = NULL, *argsJ = afb_request_json(request);

    SoftMixerT *mixer = handle->mixer;
    AlsaSndCtlT *sndcard = handle->sndcard;
    assert(mixer && sndcard);

    error = wrap_json_unpack(argsJ, "{s?b s?b,s?b,s?b,s?o,s?o !}"
            , "quiet", &doQuiet
            , "close", &doClose
            , "mute", &doMute
            , "toggle", &doToggle
            , "volume", &volumeJ
            , "ramp", &rampJ
            );
    if (error) {
        AFB_ReqFailF(request, "syntax-error", "Missing 'close|mute|volume|quiet' args=%s", json_object_get_string(argsJ));
        goto OnErrorExit;
    }

    if (doClose) {
        AFB_ReqFailF(request, "internal-error", "(Fulup) Close action still to be done mixer=%s stream=%s", mixer->uid, handle->stream->uid);
        goto OnErrorExit;
    }

    if (doToggle) {
        error += AlsaCtlNumidGetLong(mixer, sndcard, handle->stream->mute, &mute);
        error += AlsaCtlNumidSetLong(mixer, sndcard, handle->stream->mute, !mute);
    }

    if (volumeJ) {
        long curvol, newvol;
        const char*volString;

        error = AlsaCtlNumidGetLong(mixer, handle->sndcard, handle->stream->volume, &curvol);
        if (error) {
            AFB_ReqFailF(request, "invalid-numid", "Fail to get volume numid=%d value=%ld", handle->stream->volume, volume);
            goto OnErrorExit;
        }


        switch (json_object_get_type(volumeJ)) {
            case json_type_string:
                volString = json_object_get_string(volumeJ);
                switch (volString[0]) {
                    case '+':
                        sscanf(&volString[1], "%ld", &newvol);
                        newvol = curvol + newvol;
                        break;

                    case '-':
                        sscanf(&volString[1], "%ld", &newvol);
                        newvol = curvol - newvol;
                        break;
                    default:
                        error = sscanf(&volString[0], "%ld", &newvol);
                        if (error != 1) {
                            AFB_ReqFailF(request, "not-integer", "relative volume should start by '+|-' value=%s", json_object_get_string(volumeJ));
                            goto OnErrorExit;
                        }
                }
                break;
            case json_type_int:
                newvol = json_object_get_int(volumeJ);
                break;
            default:
                AFB_ReqFailF(request, "not-integer", "volume should be string or integer value=%s", json_object_get_string(volumeJ));
                goto OnErrorExit;

        }

        error = AlsaCtlNumidSetLong(mixer, handle->sndcard, handle->stream->volume, newvol);
        if (error) {
            AFB_ReqFailF(request, "StreamApiVerbCB", "Fail to set stream volume numid=%d value=%ld", handle->stream->volume, newvol);
            goto OnErrorExit;
        }
    }

    if (rampJ) {
        error = AlsaVolRampApply(mixer, handle->sndcard, handle->stream, rampJ);
        if (error) {
            AFB_ReqFailF(request, "StreamApiVerbCB", "Fail to set stream volram numid=%d value=%s", handle->stream->volume, json_object_get_string(rampJ));
            goto OnErrorExit;
        }
    }

    if (doMute != -1) {
        error = AlsaCtlNumidSetLong(mixer, handle->sndcard, handle->stream->mute, !mute);
        if (error) {
            AFB_ReqFailF(request, "StreamApiVerbCB", "Fail to set stream volume numid=%d value=%d", handle->stream->volume, !mute);
            goto OnErrorExit;
        }
    }

    // if not in quiet mode return effective selected control values
    if (doQuiet) responseJ = NULL;
    else {
        error += AlsaCtlNumidGetLong(mixer, handle->sndcard, handle->stream->volume, &volume);
        error += AlsaCtlNumidGetLong(mixer, handle->sndcard, handle->stream->mute, &mute);
        if (error) {
            AFB_ReqFailF(request, "StreamApiVerbCB", "Fail to get stream numids volume=%ld mute=%ld", volume, mute);
            goto OnErrorExit;
        }
        wrap_json_pack(&responseJ, "{si,sb}", "volume", volume, "mute", !mute);
    }

    AFB_ReqSucess(request, responseJ, NULL);
    return;

OnErrorExit:
    return;
}

PUBLIC json_object *CreateOneStream(SoftMixerT *mixer, AlsaStreamAudioT *stream) {
    int error;
    long value;
    AlsaSndLoopT *loop = NULL;
    AlsaPcmCtlT *streamPcm;
    AlsaSndCtlT *captureCard;
    AlsaDevInfoT *captureDev = alloca(sizeof (AlsaDevInfoT));
    AlsaLoopSubdevT *loopDev;
    char * captureName;

    loopDev = ApiLoopFindSubdev(mixer, stream->uid, stream->source, &loop);
    if (loopDev) {
        // create a valid PCM reference and try to open it.
        captureDev->devpath = NULL;
        captureDev->cardid = NULL;
        captureDev->cardidx = loop->sndcard->cid.cardidx;
        captureDev->device = loop->capture;
        captureDev->subdev = loopDev->index;
        captureCard = loop->sndcard;
    } else {
        // if capture UID is not present in loop search on sources
        AlsaSndCtlT *sourceDev = ApiSourceFindSubdev(mixer, stream->source);
        if (sourceDev) {
            captureDev->devpath = NULL;
            captureDev->cardid = NULL;
            captureDev->cardidx = sourceDev->cid.cardidx;
            captureDev->device = sourceDev->cid.device;
            captureDev->subdev = sourceDev->cid.subdev;
            captureCard = sourceDev;
        } else {
            AFB_ApiError(mixer->api, "CreateOneStream: mixer=%s stream=%s not found in loops/sources", mixer->uid, stream->uid);
            goto OnErrorExit;
        }
    }

    // check PCM is valid and get its full name
    AlsaPcmCtlT *capturePcm = AlsaByPathOpenPcm(mixer, captureDev, SND_PCM_STREAM_CAPTURE);
    if (!capturePcm) goto OnErrorExit;

    // Registry capturePcm PCM for active/pause event
    if (loopDev && loopDev->numid) {
        error = AlsaCtlRegister(mixer, captureCard, capturePcm, FONTEND_NUMID_RUN, loopDev->numid);
        if (error) goto OnErrorExit;
    }

    AlsaSndZoneT *zone = ApiZoneGetByUid(mixer, stream->sink);
    if (!zone) {
        AFB_ApiError(mixer->api, "CreateOneStream: mixer=%s stream=%s fail to find sink zone='%s'", mixer->uid, stream->uid, stream->sink);
        goto OnErrorExit;
    }

    // retrieve channel count from route and push it to stream
    stream->params->channels = zone->ccount;

    // create mute control and Registry it as pause/resume ctl)
    char *runName;
    (void) asprintf(&runName, "pause-%s", stream->uid);
    int pauseNumid = AlsaCtlCreateControl(mixer, captureCard, runName, 1, 0, 1, 1, stream->mute);
    if (pauseNumid <= 0) goto OnErrorExit;

    // Registry stop/play as a pause/resume control
    error = AlsaCtlRegister(mixer, captureCard, capturePcm, FONTEND_NUMID_PAUSE, pauseNumid);
    if (error) goto OnErrorExit;


    char *volName;
    (void) asprintf(&volName, "vol-%s", stream->uid);

    // create stream and delay pcm opening until vol control is created
    streamPcm = AlsaCreateSoftvol(mixer, stream, zone, captureCard, volName, VOL_CONTROL_MAX, 0);
    if (!streamPcm) {
        AFB_ApiError(mixer->api, "CreateOneStream: mixer=%s stream=%s fail to create stream", mixer->uid, stream->uid);
        goto OnErrorExit;
    }

    // create volume control before softvol pcm is opened
    int volNumid = AlsaCtlCreateControl(mixer, captureCard, volName, stream->params->channels, VOL_CONTROL_MIN, VOL_CONTROL_MAX, VOL_CONTROL_STEP, stream->volume);
    if (volNumid <= 0) goto OnErrorExit;

    if ((zone->params->rate != stream->params->rate) || (zone->params->format != stream->params->format)) {
        char *rateName;
        (void) asprintf(&rateName, "rate-%s", stream->uid);
        streamPcm = AlsaCreateRate(mixer, rateName, streamPcm, zone->params, 0);
        if (!streamPcm) {
            AFB_ApiError(mixer->api, "StreamsAttach: mixer=%s stream=%s fail to create rate converter", mixer->uid, stream->uid);
            goto OnErrorExit;
        }
        captureName = rateName;
    } else {
        captureName = (char*) streamPcm->cid.cardid;
    }

    // everything is not ready to open playback pcm
    error = snd_pcm_open(&streamPcm->handle, captureName, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (error) {
        AFB_ApiError(mixer->api, "CreateOneStream: mixer=%s stream=%s fail to open capturePcm=%s error=%s", mixer->uid, stream->uid, streamPcm->cid.cardid, snd_strerror(error));
        goto OnErrorExit;
    }

    // start stream pcm copy (at this both capturePcm & sink pcm should be open, we use output params to configure both in+outPCM)
    error = AlsaPcmCopy(mixer, stream, capturePcm, streamPcm, stream->params);
    if (error) goto OnErrorExit;

    error = AlsaCtlRegister(mixer, captureCard, capturePcm, FONTEND_NUMID_IGNORE, volNumid);
    if (error) goto OnErrorExit;

    // when using loopdev check if subdev is active or not to prevent thread from reading empty packet
    if (loopDev && loopDev->numid) {
        // retrieve active/pause control and set PCM status accordingly 
        error = AlsaCtlNumidGetLong(mixer, captureCard, loopDev->numid, &value);
        if (error) goto OnErrorExit;

        // toggle pause/resume (should be done after pcm_start)
        if ((error = snd_pcm_pause(capturePcm->handle, !value)) < 0) {
            AFB_ApiWarning(mixer->api, "CreateOneStream: mixer=%s [capturePcm=%s] fail to pause error=%s", mixer->uid, captureDev->cardid, snd_strerror(error));
        }
    }

    // prepare response for application
    json_object *paramsJ, *streamJ, *numidsJ;
    char *appCardId = NULL;

    // return alsa URI only when loopback is used
    if (loop) {
        (void) asprintf(&appCardId, "hw:%d,%d,%d", captureDev->cardidx, loop->playback, capturePcm->cid.subdev);
    } else {
        appCardId = "";
    }

    
    error += wrap_json_pack(&paramsJ, "{si si si si}"
            , "rate", stream->params->rate
            , "channels", stream->params->channels
            , "format", stream->params->format
            , "access", stream->params->access
            );
    error += wrap_json_pack(&numidsJ, "{si si}"
            , "volume", volNumid
            , "pause", pauseNumid
            );
    error += wrap_json_pack(&streamJ, "{ss ss so so}"
            , "uid", stream->uid
            , "alsa", appCardId
            , "numid", numidsJ
            , "params", paramsJ
            );
    if (error) {
        AFB_ApiError(mixer->api, "CreateOneStream: mixer=%s stream=%s fail to prepare response", mixer->uid, stream->uid);
        goto OnErrorExit;
    }

    // create a dedicated verb for this stream 
    apiHandleT *apiHandle = calloc(1, sizeof (apiHandleT));

    apiHandle->mixer = mixer;
    apiHandle->stream = stream;
    apiHandle->sndcard = captureCard;
    apiHandle->pcm = capturePcm->handle;
    
    // replace stream volume/mute values with corresponding ctl control
    stream->volume=volNumid;
    stream->mute=pauseNumid;

    error = afb_dynapi_add_verb(mixer->api, stream->uid, stream->info, StreamApiVerbCB, apiHandle, NULL, 0);
    if (error) {
        AFB_ApiError(mixer->api, "CreateOneStream mixer=%s fail to Register API verb stream=%s", mixer->uid, stream->uid);
        goto OnErrorExit;
    }

    // Debug Alsa Config 
    //AlsaDumpElemConfig (source, "\n\nAlsa_Config\n------------\n", "pcm");
    //AlsaDumpPcmInfo(source, "\n\nPcm_config\n-----------\n", streamPcm->handle);

    AFB_ApiNotice(mixer->api, "CreateOneStream: mixer=%s stream=%s OK reponse=%s\n", mixer->uid, stream->uid, json_object_get_string(streamJ));

    return streamJ;

OnErrorExit:
    return NULL;
}

STATIC AlsaStreamAudioT * AttachOneStream(SoftMixerT *mixer, const char *uid, json_object *streamJ, json_object **responseJ) {
    AlsaStreamAudioT *stream = calloc(1, sizeof (AlsaStreamAudioT));
    int error;
    json_object *paramsJ = NULL;

    // Make sure default runs
    stream->volume = ALSA_DEFAULT_PCM_VOLUME;
    stream->mute = 0;
    stream->info = NULL;

    error = wrap_json_unpack(streamJ, "{ss,s?s,ss,s?s,s?i,s?b,s?o,s?s !}"
            , "uid", &stream->uid
            , "info", &stream->info
            , "zone", &stream->sink
            , "source", &stream->source
            , "volume", &stream->volume
            , "mute", &stream->mute
            , "params", &paramsJ
            , "ramp", &stream->ramp
            );

    if (error) {
        AFB_ApiNotice(mixer->api, "ProcessOneStream hal=%s missing 'uid|[info]|zone|source||[volume]|[mute]|[params]' error=%s stream=%s", uid, wrap_json_get_error_string(error), json_object_get_string(streamJ));
        goto OnErrorExit;
    }

    stream->params = ApiPcmSetParams(mixer, stream->uid, paramsJ);
    if (!stream->params) {
        AFB_ApiError(mixer->api, "ProcessOneSndCard: hal=%s stream=%s invalid params=%s", uid, stream->uid, json_object_get_string(paramsJ));
        goto OnErrorExit;
    }

    // make sure remain valid even when json object is removed
    stream->uid = strdup(stream->uid);
    if (stream->sink)stream->sink = strdup(stream->sink);
    if (stream->source)stream->source = strdup(stream->source);

    // implement stream PCM with corresponding thread and controls
    *responseJ = CreateOneStream(mixer, stream);

    return stream;

OnErrorExit:
    return NULL;
}

PUBLIC int ApiStreamAttach(SoftMixerT *mixer, AFB_ReqT request, const char *uid, json_object * argsJ, json_object **responseJ) {

    if (!mixer->loops) {
        AFB_ApiError(mixer->api, "StreamsAttach: mixer=%s No Loop found [should Registry snd_loop first]", mixer->uid);
        goto OnErrorExit;
    }

    int index;
    for (index = 0; index < mixer->max.streams; index++) {
        if (!mixer->streams[index]) break;
    }

    if (index == mixer->max.streams) {
        AFB_ReqFailF(request, "too-small", "mixer=%s max stream=%d argsJ= %s", mixer->uid, mixer->max.streams, json_object_get_string(argsJ));
        goto OnErrorExit;
    }

    switch (json_object_get_type(argsJ)) {
            long count;

        case json_type_object:
            mixer->streams[index] = AttachOneStream(mixer, uid, argsJ, responseJ);
            if (!mixer->streams[index]) {
                AFB_ReqFailF(request, "invalid-syntax", "mixer=%s invalid stream= %s", mixer->uid, json_object_get_string(argsJ));
                goto OnErrorExit;
            }
            break;

        case json_type_array:
            *responseJ = json_object_new_array();

            count = json_object_array_length(argsJ);
            if (count > (mixer->max.streams - count)) {
                AFB_ReqFailF(request, "too-small", "mixer=%s max stream=%d argsJ= %s", mixer->uid, mixer->max.streams, json_object_get_string(argsJ));
                goto OnErrorExit;
            }

            for (int idx = 0; idx < count; idx++) {
                json_object *streamJ = json_object_array_get_idx(argsJ, idx);
                mixer->streams[index + idx] = AttachOneStream(mixer, uid, streamJ, &streamJ);
                if (!mixer->streams[index + idx]) {
                    AFB_ReqFailF(request, "invalid-syntax", "mixer=%s invalid stream= %s", mixer->uid, json_object_get_string(streamJ));
                    goto OnErrorExit;
                }
                (void) json_object_array_add(*responseJ, streamJ);
            }
            break;
        default:
            AFB_ReqFailF(request, "invalid-syntax", "mixer=%s streams invalid argsJ= %s", mixer->uid, json_object_get_string(argsJ));
            goto OnErrorExit;
    }

    return 0;

OnErrorExit:
    return -1;
}
