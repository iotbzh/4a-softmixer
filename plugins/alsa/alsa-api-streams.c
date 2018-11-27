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
#include "alsa-bluez.h"

#include <avirt/avirt.h>

#include <string.h>
#include <stdbool.h>
#include <dlfcn.h>

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
    apiHandleT *handle = (apiHandleT*) afb_req_get_vcbdata(request);
    int error, verbose = 0, doClose = 0, doToggle = 0, doMute = -1, doInfo = 0;
    long mute, volume, curvol;
    json_object *volumeJ = NULL, *rampJ = NULL, *argsJ = afb_req_json(request);
    json_object *responseJ = NULL;
    SoftMixerT *mixer = handle->mixer;
    AlsaSndCtlT *sndcard = handle->sndcard;
    assert(mixer && sndcard);

    error = wrap_json_unpack(argsJ, "{s?b s?b,s?b,s?b,s?b,s?o,s?o !}"
            , "close", &doClose
            , "mute", &doMute
            , "toggle", &doToggle
            , "info", &doInfo
            , "verbose", &verbose
            , "volume", &volumeJ
            , "ramp", &rampJ
            );

    if (error) {
        AFB_ReqFailF(request, "syntax-error", "Missing 'close|mute|volume|verbose' args=%s", json_object_get_string(argsJ));
        goto OnErrorExit;
    }

    if (verbose) responseJ = json_object_new_object();

    if (doClose) {
        AFB_ReqFailF(request, "internal-error", "(Fulup) Close action still to be done mixer=%s stream=%s", mixer->uid, handle->stream->uid);
        goto OnErrorExit;
    }

    if (doToggle) {
        error += AlsaCtlNumidGetLong(mixer, sndcard, handle->stream->mute, &mute);
        error += AlsaCtlNumidSetLong(mixer, sndcard, handle->stream->mute, !mute);
        if (error) {
            AFB_ReqFailF(request, "invalid-numid", "Fail to set/get pause numid=%d", handle->stream->mute);
            goto OnErrorExit;
        }

        if (verbose) {
            json_object_object_add(responseJ, "mute", json_object_new_boolean(!mute));
        }
    }

    if (doMute != -1) {
        error = AlsaCtlNumidSetLong(mixer, handle->sndcard, handle->stream->mute, doMute);
        if (error) {
            AFB_ReqFailF(request, "StreamApiVerbCB", "Fail to set stream volume numid=%d value=%d", handle->stream->volume, !doMute);
            goto OnErrorExit;
        }

        if (verbose) {
            error += AlsaCtlNumidGetLong(mixer, handle->sndcard, handle->stream->mute, &mute);
            json_object_object_add(responseJ, "mute", json_object_new_boolean((json_bool) mute));
        }
    }

    if (volumeJ) {
        long newvol;
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

        if (verbose) {
            json_object_object_add(responseJ, "volnew", json_object_new_int((int) newvol));
            json_object_object_add(responseJ, "volold", json_object_new_int((int) curvol));
        }
    }

    if (rampJ) {
        if (verbose) {
            error = AlsaCtlNumidGetLong(mixer, handle->sndcard, handle->stream->volume, &curvol);
            json_object_object_add(responseJ, "volold", json_object_new_int((int) curvol));
        }

        error += AlsaVolRampApply(mixer, handle->sndcard, handle->stream, rampJ);
        if (error) {
            AFB_ReqFailF(request, "StreamApiVerbCB", "Fail to set stream volram numid=%d value=%s", handle->stream->volume, json_object_get_string(rampJ));
            goto OnErrorExit;
        }
    }


    if (doInfo) {
        json_object_put(responseJ); // free default response.
        error += AlsaCtlNumidGetLong(mixer, handle->sndcard, handle->stream->volume, &volume);
        error += AlsaCtlNumidGetLong(mixer, handle->sndcard, handle->stream->mute, &mute);
        if (error) {
            AFB_ReqFailF(request, "StreamApiVerbCB", "Fail to get stream numids volume=%ld mute=%ld", volume, mute);
            goto OnErrorExit;
        }
        wrap_json_pack(&responseJ, "{si,sb}", "volume", volume, "mute", !mute);
    }

    AFB_ReqSuccess(request, responseJ, NULL);
    return;

OnErrorExit:
    return;
}

STATIC int CreateOneStream(SoftMixerT *mixer, const char * uid, AlsaStreamAudioT * stream) {
    int error;
    long value;
    AlsaSndLoopT *loop = NULL;
    AlsaPcmCtlT *streamPcm;
    AlsaSndCtlT *captureCard;
    AlsaDevInfoT *captureDev = alloca(sizeof (AlsaDevInfoT));
    AlsaLoopSubdevT *loopDev;
    AlsaSndZoneT *zone;
    char *volSlaveId = NULL;
    char *playbackName = NULL;
    char *runName = NULL;
    char *volName = NULL;
    int pauseNumid = 0;
    int volNumid = 0;
    int device, subdev;

    AFB_ApiInfo(mixer->api,
                "%s, stream %s %s, source %s, sink %s, mute %d",
                __func__,uid, stream->uid, stream->source, stream->sink, stream->mute);

    loopDev = ApiLoopFindSubdev(mixer, stream->uid, stream->source, &loop);
    if (loopDev) {
        if (loop->avirt) {
            device = loopDev->index;
            subdev = 0;
        } else { // loop->avirt == false
            device = loop->capture;
            subdev = loopDev->index;
        }

        // create a valid PCM reference and try to open it.
        captureDev->devpath = NULL;
        captureDev->cardid = NULL;
        captureDev->cardidx = loop->sndcard->cid.cardidx;
        captureDev->device = device;
        captureDev->subdev = subdev;
        captureDev->pcmplug_params = NULL;
        captureCard = loop->sndcard;

        AFB_ApiInfo(mixer->api,
                    "%s: found loopdev %d,%d",
                    __func__, loop->capture, loopDev->index);

    } else {
        // if capture UID is not present in loop search on sources
        AFB_ApiInfo(mixer->api,"%s: %s not found in loop, look in sources", __func__, uid);

        AlsaSndCtlT *sourceDev = ApiSourceFindSubdev(mixer, stream->source);
        if (sourceDev) {
            captureDev->devpath = NULL;
            captureDev->cardid = NULL;
            captureDev->cardidx = sourceDev->cid.cardidx;
            captureDev->device = sourceDev->cid.device;
            captureDev->subdev = sourceDev->cid.subdev;
            captureDev->pcmplug_params = sourceDev->cid.pcmplug_params;
            captureCard = sourceDev;
            AFB_ApiInfo(mixer->api, "%s found capture %s", __func__, uid);
        } else {
            AFB_ApiError(mixer->api,
                         "%s: mixer=%s stream=%s not found in loops/sources",
                         __func__, mixer->uid, stream->uid);
            goto OnErrorExit;
        }
    }

    // check PCM is valid and get its full name
    AlsaPcmCtlT *capturePcm = AlsaByPathOpenPcm(mixer, captureDev, SND_PCM_STREAM_CAPTURE);
    if (!capturePcm) goto OnErrorExit;

    capturePcm->mute = stream->mute;

    AFB_ApiInfo(mixer->api,"%s: PCM opened !", __func__);

    // Registry capturePcm PCM for active/pause event
    if (loopDev && loopDev->numid) {
        error = AlsaCtlRegister(mixer, captureCard, capturePcm, FONTEND_NUMID_RUN, loopDev->numid);
        if (error) goto OnErrorExit;
    }

    if (mixer->zones[0]) {
        // if zones exist then retrieve zone pcmid and channel count
        zone = ApiZoneGetByUid(mixer, stream->sink);
        if (!zone) {
            AFB_ApiError(mixer->api,
                         "%s: mixer=%s stream=%s fail to find sink zone='%s'",
                         __func__, mixer->uid, stream->uid, stream->sink);
            goto OnErrorExit;
        }

        // route PCM should have been create during zones attach phase.
        if (asprintf(&volSlaveId, "route-%s", zone->uid) == -1)
            goto OnErrorExit;

    } else {
        AlsaSndPcmT *playback = ApiSinkGetByUid(mixer, stream->sink);
        if (!playback) {
            AFB_ApiError(mixer->api,
                         "%s: mixer=%s stream=%s fail to find sink playback='%s'",
						 __func__, mixer->uid, stream->uid, stream->sink);
            goto OnErrorExit;
        }

        // retrieve channel count from route and push it to stream
        if (asprintf(&volSlaveId, "dmix-%s", playback->uid) == -1)
            goto OnErrorExit;
        
        // create a fake zone for rate converter selection
        zone=alloca(sizeof(AlsaSndZoneT));
        zone->uid= playback->uid;
        zone->params = playback->sndcard->params;
        zone->ccount = playback->ccount;
    }

    // retrieve channel count from route and push it to stream

    stream->params->channels = zone->ccount;

    // create mute control and Registry it as pause/resume ctl)
    if (asprintf(&runName, "pause-%s", stream->uid) == -1)
        goto OnErrorExit;

    AFB_ApiInfo(mixer->api,"%s: create mute control...", __func__);

    pauseNumid = AlsaCtlCreateControl(mixer, captureCard, runName, 1, 0, 1, 1, stream->mute);
    if (pauseNumid <= 0) {
        AFB_ApiError(mixer->api, "%s: Failed to create pause control", __func__);
        goto OnErrorExit;
    }

    AFB_ApiInfo(mixer->api,"%s: register mute control...", __func__);

    // Registry stop/play as a pause/resume control
    error = AlsaCtlRegister(mixer, captureCard, capturePcm, FONTEND_NUMID_PAUSE, pauseNumid);
    if (error) {
        AFB_ApiError(mixer->api, "%s: Failed to register pause control", __func__);
        goto OnErrorExit;
    }

    if (asprintf(&volName, "vol-%s", stream->uid) == -1)
        goto OnErrorExit;

    AFB_ApiInfo(mixer->api,"%s: create softvol", __func__);

    // create stream and delay pcm opening until vol control is created
    streamPcm = AlsaCreateSoftvol(mixer, stream, volSlaveId, captureCard, volName, VOL_CONTROL_MAX, 0);
    if (!streamPcm) {
        AFB_ApiError(mixer->api, "%s failed to create soft volume PCM", __func__);
        goto OnErrorExit;
    }

    AFB_ApiInfo(mixer->api,"%s: create softvol control", __func__);

    // create volume control before softvol pcm is opened
    volNumid = AlsaCtlCreateControl(mixer,
                                        captureCard,
                                        volName,
                                        stream->params->channels,
                                        VOL_CONTROL_MIN,
                                        VOL_CONTROL_MAX,
                                        VOL_CONTROL_STEP,
                                        stream->volume);
    if (volNumid <= 0) {
        AFB_ApiError(mixer->api, "%s failed add volume control on capture card", __func__);
        goto OnErrorExit;
    }

    if ((zone->params->rate   != stream->params->rate) ||
        (zone->params->format != stream->params->format)) {
        AFB_ApiNotice(mixer->api,
                      "%s: Instanciate a RATE CONVERTER (stream [%d,%s(%d)], zone [%d,%s(%d)])",
                      __func__,
                      stream->params->rate,
                      stream->params->formatS,
                      stream->params->format,
                      zone->params->rate,
                      zone->params->formatS,
                      zone->params->format);

        char *rateName;
        if (asprintf(&rateName, "rate-%s", stream->uid) == -1)
            goto OnErrorExit;
        streamPcm = AlsaCreateRate(mixer, rateName, streamPcm, zone->params, 0);
        if (!streamPcm) {
            AFB_ApiError(mixer->api, "%s: fail to create rate converter", __func__);
            goto OnErrorExit;
        }
        playbackName = rateName;
    } else {
        AFB_ApiNotice(mixer->api, "%s: no need for a converter", __func__);
        playbackName = (char*) streamPcm->cid.cardid;
    }

    AFB_ApiInfo(mixer->api, "%s: Opening PCM PLAYBACK name %s", __func__, playbackName);

    // everything is now ready to open playback pcm in BLOCKING mode this time
    error = snd_pcm_open(&streamPcm->handle, playbackName, SND_PCM_STREAM_PLAYBACK, 0 /* will block*/ );
    if (error) {
        AFB_ApiError(mixer->api,
                     "%s: mixer=%s stream=%s fail to open playback PCM=%s; error=%s",
                     __func__, mixer->uid, stream->uid, streamPcm->cid.cardid, snd_strerror(error));
        goto OnErrorExit;
    }

    // start stream pcm copy (at this both capturePcm & sink pcm should be open, we use output params to configure both in+outPCM)
    error = AlsaPcmCopy(mixer, stream, capturePcm, streamPcm, stream->params);
    if (error) {
        AFB_ApiError(mixer->api, "%s: Failed to launch copy", __func__);
        goto OnErrorExit;
    }

    error = AlsaCtlRegister(mixer, captureCard, capturePcm, FONTEND_NUMID_IGNORE, volNumid);
    if (error) {
        AFB_ApiError(mixer->api, "%s: register control on capture", __func__);
        goto OnErrorExit;
    }

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

    if (loop) {
        int device = (loop->avirt) ? captureDev->device
                                   : loop->playback;
        if (asprintf((char**) &stream->source, "hw:%d,%d,%d", captureDev->cardidx, device, capturePcm->cid.subdev) == -1)
            goto OnErrorExit;
    } else {
        if (asprintf((char**) &stream->source, "hw:%d,%d,%d", captureDev->cardidx, captureDev->device, captureDev->subdev) == -1)
            goto OnErrorExit;
    }

    // create a dedicated verb for this stream 
    apiHandleT *apiHandle = calloc(1, sizeof (apiHandleT));

    apiHandle->mixer = mixer;
    apiHandle->stream = stream;
    apiHandle->sndcard = captureCard;
    apiHandle->pcm = capturePcm->handle;

    // replace stream volume/mute values with corresponding ctl control
    stream->volume = volNumid;
    stream->mute = pauseNumid;

    error = afb_api_add_verb(mixer->api, stream->verb, stream->info, StreamApiVerbCB, apiHandle, NULL, 0, 0);
    if (error) {
        AFB_ApiError(mixer->api,
                     "%s mixer=%s fail to Register API verb stream=%s",
                     __func__, mixer->uid, stream->uid);
        goto OnErrorExit;
    }

    // Debug Alsa Config 
    //AlsaDumpElemConfig (source, "\n\nAlsa_Config\n------------\n", "pcm");
    //AlsaDumpPcmInfo(source, "\n\nPcm_config\n-----------\n", streamPcm->handle);

    AFB_ApiNotice(mixer->api,
                  "%s: mixer=%s stream=%s done",
                  __func__, mixer->uid, stream->uid);

    return 0;

OnErrorExit:
	free(volSlaveId);
	free(runName);
	free(volName);
    return -1;
}

STATIC AlsaStreamAudioT * AttachOneStream(SoftMixerT *mixer, const char *uid, const char *prefix, json_object * streamJ) {
    AlsaStreamAudioT *stream = calloc(1, sizeof (AlsaStreamAudioT));
    int error;
    json_object *paramsJ = NULL;

    // Make sure default runs
    stream->volume = ALSA_DEFAULT_PCM_VOLUME;
    stream->mute = 0;
    stream->info = NULL;

    error = wrap_json_unpack(streamJ, "{ss,s?s,s?s,ss,s?s,s?i,s?b,s?o,s?s !}"
            , "uid", &stream->uid
            , "verb", &stream->verb
            , "info", &stream->info
            , "zone", &stream->sink
            , "source", &stream->source
            , "volume", &stream->volume
            , "mute", &stream->mute
            , "params", &paramsJ
            , "ramp", &stream->ramp
            );

    if (error) {
        AFB_ApiNotice(mixer->api,
                       "%s: hal=%s missing 'uid|[info]|zone|source||[volume]|[mute]|[params]' error=%s stream=%s",
                       __func__, uid, wrap_json_get_error_string(error), json_object_get_string(streamJ));
        goto OnErrorExit;
    }

    stream->params = ApiPcmSetParams(mixer, stream->uid, paramsJ);
    if (!stream->params) {
        AFB_ApiError(mixer->api,
                     "%s: hal=%s stream=%s invalid params=%s",
                     __func__, uid, stream->uid, json_object_get_string(paramsJ));
        goto OnErrorExit;
    }

    // make sure remain valid even when json object is removed
    stream->uid = strdup(stream->uid);
    if (stream->sink)stream->sink = strdup(stream->sink);
    if (stream->source)stream->source = strdup(stream->source);

    // Prefix verb with uid|prefix
    if (prefix) {
        if (stream->verb) {
            if (asprintf((char**) &stream->verb, "%s#%s", prefix, stream->verb) == -1)
                goto OnErrorExit;
        }
        else {
            if (asprintf((char**) &stream->verb, "%s#%s", prefix, stream->uid) == -1)
                goto OnErrorExit;
        }
    } else {
        if (!stream->verb)
            stream->verb = strdup(stream->uid);
    }

    // implement stream PCM with corresponding thread and controls
    error = CreateOneStream(mixer, uid, stream);
    if (error) {
        AFB_ApiError(mixer->api, "%s: failed to create stream", __func__);
        goto OnErrorExit;
    }

    return stream;

OnErrorExit:
    free(stream);
    AFB_ApiError(mixer->api, "%s fail", __func__);
    return NULL;
}

PUBLIC int ApiStreamAttach(SoftMixerT *mixer, AFB_ReqT request, const char *uid, const char *prefix, json_object * argsJ) {

	AFB_ApiInfo(mixer->api, "%s: %s prefix %s", __func__, uid, prefix);

    if (!mixer->loops) {
        AFB_ApiError(mixer->api, "%s: mixer=%s No Loop found [should Registry snd_loop first]", __func__, mixer->uid);
        goto OnErrorExit;
    }

    int index;
    for (index = 0; index < mixer->max.streams; index++) {
        if (!mixer->streams[index]) break;
    }

    if (index == mixer->max.streams) {
        AFB_ReqFailF(request, "too-small", "mixer=%s max stream=%d", mixer->uid, mixer->max.streams);
        goto OnErrorExit;
    }

    switch (json_object_get_type(argsJ)) {
        long count;

        case json_type_object:
            mixer->streams[index] = AttachOneStream(mixer, uid, prefix, argsJ);
            if (!mixer->streams[index]) {
                AFB_ReqFailF(request, "bad-stream", "mixer=%s invalid stream= %s", mixer->uid, json_object_get_string(argsJ));
                goto OnErrorExit;
            }
            break;

        case json_type_array:

            count = json_object_array_length(argsJ);
            if (count > (mixer->max.streams - index)) {
                AFB_ReqFailF(request,
                             "too-small",
                             "mixer=%s max stream=%d", mixer->uid, mixer->max.streams);
                goto OnErrorExit;
            }

            for (int idx = 0; idx < count; idx++) {
                json_object *streamJ = json_object_array_get_idx(argsJ, idx);
                mixer->streams[index + idx] = AttachOneStream(mixer, uid, prefix, streamJ);
                if (!mixer->streams[index + idx]) {
                    AFB_ReqFailF(request,
                                 "bad-stream",
                                 "%s: mixer=%s invalid stream= %s",
                                 __func__, mixer->uid, json_object_get_string(streamJ));
                    goto OnErrorExit;
                }
            }
            break;
        default:
            AFB_ReqFailF(request,
                         "invalid-syntax",
                         "mixer=%s streams invalid argsJ= %s", mixer->uid, json_object_get_string(argsJ));
            goto OnErrorExit;
    }

    return 0;

OnErrorExit:
	AFB_ApiError(mixer->api, "%s FAILED\n", __func__);
    return -1;
}
