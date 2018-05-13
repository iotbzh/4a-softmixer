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

// Fulup need to be cleanup with new controller version
extern Lua2cWrapperT Lua2cWrap;

STATIC int ProcessOneChannel(CtlSourceT *source, const char* uid, json_object *channelJ, AlsaPcmChannels *channel) {
    const char*channelUid;
    
    int error = wrap_json_unpack(channelJ, "{ss,si !}", "uid", &channelUid, "port", &channel->port);
    if (error) goto OnErrorExit;

    channel->uid=strdup(channelUid);
    return 0;

OnErrorExit:
    AFB_ApiError(source->api, "ProcessOneChannel: sndcard=%s channel: missing (uid||port) json=%s", uid, json_object_get_string(channelJ));    
    return -1;
}

STATIC int ProcessSndParams(CtlSourceT *source, const char* uid, json_object *paramsJ, AlsaPcmHwInfoT *params) {
    
    int error = wrap_json_unpack(paramsJ, "{s?i,s?i !}", "rate", &params->rate, "channels", &params->channels);
    if (error) goto OnErrorExit;

    return 0;

OnErrorExit:
    AFB_ApiError(source->api, "ProcessSndParams: sndcard=%s params: missing (rate|channel) params=%s", uid, json_object_get_string(paramsJ));    
    return -1;
}

STATIC int ProcessOneSndCard(CtlSourceT *source, json_object *sndcardJ, AlsaPcmInfoT *snd) {
    json_object *sinkJ, *paramsJ=NULL;
    int error;

    error = wrap_json_unpack(sndcardJ, "{ss,s?s,s?s,s?i,s?i,s?i,so,s?o !}", "uid",&snd->uid, "devpath",&snd->devpath, "cardid",&snd->cardid
              , "cardidx",&snd->cardidx, "device",&snd->device, "subdev",&snd->subdev, "sink",&sinkJ, "params",&paramsJ);
    if (error || !snd->uid || !sinkJ || (!snd->devpath && !snd->cardid && snd->cardidx)) {
        AFB_ApiNotice(source->api, "ProcessOneSndCard missing 'uid|path|cardid|cardidx|channels|device|subdev|numid|params' devin=%s", json_object_get_string(sndcardJ));
        goto OnErrorExit;
    }
    
    if (paramsJ) error= ProcessSndParams(source, snd->uid, paramsJ, &snd->params);
    if (error) {
        AFB_ApiError(source->api, "ProcessOneSndCard: sndcard=%s invalid params=%s", snd->uid, json_object_get_string(paramsJ));
        goto OnErrorExit;
    }
        
    
    // check snd card is accessible
    error = AlsaByPathDevid(source, snd);
    if (error) {
        AFB_ApiError(source->api, "ProcessOneSndCard: sndcard=%s not found config=%s", snd->uid, json_object_get_string(sndcardJ));
        goto OnErrorExit;
    }
    
    // protect each sndcard with a dmix plugin to enable audio-stream mixing
    char dmixUid[100];
    snprintf(dmixUid, sizeof(dmixUid),"Dmix-%s", snd->uid);
    AlsaPcmInfoT *dmixPcm= AlsaCreateDmix(source, dmixUid, snd);
    if (!dmixPcm) {
        AFB_ApiError(source->api, "ProcessOneSndCard: sndcard=%s fail to attach dmix plugin", snd->uid);
        goto OnErrorExit;        
    } else {
        snd_pcm_close(dmixPcm->handle);
        snd->cardid=dmixPcm->cardid;
    }

    switch (json_object_get_type(sinkJ)) {
        case json_type_object:
            snd->ccount=1;
            snd->channels = calloc(snd->ccount+1, sizeof (AlsaPcmChannels));
            error = ProcessOneChannel(source, snd->uid, sndcardJ, &snd->channels[0]);
            if (error) goto OnErrorExit;
            break;
        case json_type_array:
            snd->ccount = (int)json_object_array_length(sinkJ);
            snd->channels = calloc(snd->ccount+1, sizeof (AlsaPcmChannels));
            for (int idx = 0; idx < snd->ccount; idx++) {
                json_object *channelJ = json_object_array_get_idx(sinkJ, idx);
                error = ProcessOneChannel(source, snd->uid, channelJ, &snd->channels[idx]);
                if (error) goto OnErrorExit;
            }
            break;
        default:
            AFB_ApiError(source->api, "ProcessOneSndCard:%s invalid sink=%s", snd->uid, json_object_get_string(sinkJ));
            goto OnErrorExit;
    }

    return 0;

OnErrorExit:
    return -1;
}

CTLP_LUA2C(snd_cards, source, argsJ, responseJ) {
    AlsaPcmInfoT *sndcards;
 
    int error;
    size_t count;

    switch (json_object_get_type(argsJ)) {
        case json_type_object:
            count= 1;
            sndcards = calloc(count + 1, sizeof (AlsaPcmInfoT));
            error = ProcessOneSndCard(source, argsJ, &sndcards[0]);
            if (error) goto OnErrorExit;
            break;
        case json_type_array:
            count = json_object_array_length(argsJ);
            sndcards = calloc(count + 1, sizeof (AlsaPcmInfoT));
            for (int idx = 0; idx < count; idx++) {
                json_object *sndcardJ = json_object_array_get_idx(argsJ, idx);
                error = ProcessOneSndCard(source, sndcardJ, &sndcards[idx]);
                if (error) goto OnErrorExit;
            }
            break;
        default:
            AFB_ApiError(source->api, "L2C:sndcards: invalid argsJ=  %s", json_object_get_string(argsJ));
            goto OnErrorExit;
    }

    // register Sound card and multi when needed
    Softmixer->sndcardCtl= sndcards;
    
    if (count == 1) {
        
        // only one sound card we multi would be useless
        Softmixer->multiPcm = &sndcards[0];
        
    } else {
        AlsaPcmInfoT *pcmMulti;
        
        // instantiate an alsa multi plugin
        pcmMulti = AlsaCreateMulti(source, "PcmMulti");
        if (!pcmMulti) goto OnErrorExit;

        // Close Multi and save into globak handle for further use
        snd_pcm_close(pcmMulti->handle); 
        Softmixer->multiPcm= pcmMulti;
    }

    return 0;

OnErrorExit:
    AFB_ApiNotice(source->api, "L2C:sndcards fail to process: %s", json_object_get_string(argsJ));
    return -1;
}
