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


PUBLIC char *AlsaDumpPcmUid(snd_pcm_t *pcmHandle, char *buffer, size_t len) {
    snd_pcm_info_t *pcmInfo;
    snd_pcm_info_alloca(&pcmInfo);

    // retrieve PCM name for error/debug
    int error = snd_pcm_info(pcmHandle, pcmInfo);
    if (error) goto OnErrorExit;

    int pcmCard = snd_pcm_info_get_card(pcmInfo);
    const char *pcmName = snd_pcm_info_get_name(pcmInfo);
    snprintf(buffer, len, "hw:%i [%s]", pcmCard, pcmName);
    return buffer;

OnErrorExit:
    return NULL;
}

PUBLIC char *AlsaDumpCtlUid(snd_ctl_t *ctlHandle, char *buffer, size_t len) {
    snd_ctl_card_info_t *ctlInfo;

    snd_ctl_card_info_alloca(&ctlInfo);

    // retrieve PCM name for error/debug
    int error = snd_ctl_card_info(ctlHandle, ctlInfo);
    if (error) goto OnErrorExit;
    
    const char *ctlId   = snd_ctl_card_info_get_id(ctlInfo);
    const char *ctlName = snd_ctl_card_info_get_name(ctlInfo);
    snprintf(buffer, len, "hw:%s [%s]", ctlId, ctlName);
    return buffer;

OnErrorExit:
    return NULL;
}


PUBLIC void AlsaDumpFormats(CtlSourceT *source, snd_pcm_t *pcmHandle) {
    char string[32];
    snd_pcm_format_t format;
    snd_pcm_hw_params_t *pxmHwParams;

    // retrieve hadware config from PCM
    snd_pcm_hw_params_alloca(&pxmHwParams);
    snd_pcm_hw_params_any(pcmHandle, pxmHwParams);

    AFB_ApiNotice(source->api, "Available formats: PCM=%s", ALSA_PCM_UID(pcmHandle, string));
    for (format = 0; format <= SND_PCM_FORMAT_LAST; format++) {
        if (snd_pcm_hw_params_test_format(pcmHandle, pxmHwParams, format) == 0) {
            AFB_ApiNotice(source->api, "- %s", snd_pcm_format_name(format));
        }
    }
}


PUBLIC void AlsaDumpCtlSubdev(CtlSourceT *source, snd_ctl_t *handle) { 
    snd_ctl_card_info_t *cardInfo;
    int err;
    int dev= -1;
    snd_pcm_info_t *pcminfo;
    snd_pcm_info_alloca(&pcminfo);
    unsigned int subdevCount, subdevAvail;
       
    snd_ctl_card_info_alloca(&cardInfo);
    snd_ctl_card_info(handle, cardInfo);
    int cardIndex = snd_ctl_card_info_get_card(cardInfo);
    const char *cardId = snd_ctl_card_info_get_id(cardInfo);
    const char *cardName = snd_ctl_card_info_get_name(cardInfo);
    
    // loop on every sndcard devices
    while (1) {

        if (snd_ctl_pcm_next_device(handle, &dev) < 0) {
            AFB_ApiError(source->api, "AlsaDumpCard: fail to open subdev card id=%s name=%s", cardId, cardName);
            goto OnErrorExit;
        }

        // no more devices
        if (dev < 0) break;

        // ignore empty device slot
        if ((err = snd_ctl_pcm_info(handle, pcminfo)) < 0) {
            if (err != -ENOENT)
                AFB_ApiError(source->api, "control digital audio info (%s): %s", cardName, snd_strerror(err));
            continue;
        }

        AFB_ApiNotice(source->api,"AlsaDumpCard card %d: %s [%s], device %d: %s [%s]",
                cardIndex, cardId, cardName, dev, snd_pcm_info_get_id(pcminfo), snd_pcm_info_get_name(pcminfo));

        // loop on subdevices
        subdevCount = snd_pcm_info_get_subdevices_count(pcminfo);
        subdevAvail = snd_pcm_info_get_subdevices_avail(pcminfo);

        for (unsigned int idx = 0; idx < subdevCount; idx++) {
            snd_pcm_info_set_subdevice(pcminfo, idx);
            if ((err = snd_ctl_pcm_info(handle, pcminfo)) < 0) {
                AFB_ApiError(source->api, "AlsaDumpCard: control digital audio playback info %i: %s", cardIndex, snd_strerror(err));
            } else {
                AFB_ApiNotice(source->api, "AlsaDumpCard: -- Subdevice #%d: %s", idx, snd_pcm_info_get_subdevice_name(pcminfo));
            }
        }
        AFB_ApiNotice(source->api, "AlsaDumpCard  => subdevice count=%d avaliable=%d", subdevCount, subdevAvail);
    }
    return;

OnErrorExit:
    return;          
}


PUBLIC void AlsaDumpPcmParams(CtlSourceT *source, snd_pcm_hw_params_t *pcmHwParams) {
    snd_output_t *output;
    char *buffer;

    snd_output_buffer_open(&output);
    snd_pcm_hw_params_dump(pcmHwParams, output);
    snd_output_buffer_string(output, &buffer);
    AFB_ApiNotice(source->api, "AlsaPCMDump: %s", buffer);
    snd_output_close(output);
}


PUBLIC void AlsaDumpPcmInfo(CtlSourceT *source, snd_pcm_t *pcm, const char* info) {
    snd_output_t *out;
    char *buffer;

    // create an output buffer an dump PCM config
    snd_output_buffer_open(&out);
    snd_output_printf(out, info);
    snd_output_printf(out, ": ");
    snd_pcm_dump(pcm, out);

    snd_output_buffer_string(out, &buffer);
    AFB_ApiNotice(source->api, "AlsaPCMDump: %s", buffer);
    snd_output_close(out);
}

PUBLIC void AlsaDumpCtlConfig(CtlSourceT *source, snd_config_t *config, int indent) {
    snd_config_iterator_t it, next;

            // hugly hack to get minimalist indentation
            char *pretty = alloca(indent + 1);

    for (int idx = 0; idx < indent; idx++) pretty[idx] = '-';
            pretty[indent] = '\0';

            snd_config_for_each(it, next, config) {
            snd_config_t *node = snd_config_iterator_entry(it);
                    const char *key;

                    // ignore comment en empty lines
            if (snd_config_get_id(node, &key) < 0) continue;

                switch (snd_config_get_type(node)) {
                        long valueI;
                                const char *valueS;

                    case SND_CONFIG_TYPE_INTEGER:
                        snd_config_get_integer(node, &valueI);
                                AFB_ApiNotice(source->api, "DumpAlsaConfig: %s %s: %d (int)", pretty, key, (int) valueI);
                        break;

                    case SND_CONFIG_TYPE_STRING:
                        snd_config_get_string(node, &valueS);
                                AFB_ApiNotice(source->api, "DumpAlsaConfig: %s %s: %s (str)", pretty, key, valueS);
                        break;

                    case SND_CONFIG_TYPE_COMPOUND:
                        AFB_ApiNotice(source->api, "DumpAlsaConfig: %s %s { ", pretty, key);
                                AlsaDumpCtlConfig(source, node, indent + 2);
                                AFB_ApiNotice(source->api, "DumpAlsaConfig: %s } ", pretty);
                        break;

                    default:
                        snd_config_get_string(node, &valueS);
                                AFB_ApiNotice(source->api, "DumpAlsaConfig: %s: key=%s unknown=%s", pretty, key, valueS);
                        break;
                }
        }
}
