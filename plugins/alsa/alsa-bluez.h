/*
 * Copyright(C) 2018 "IoT.bzh"
 * Author Thierry Bultel <thierry.bultel@iot.bzh>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http : //www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License

 * for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef __INC_ALSA_BLUEZ_H
#define __INC_ALSA_BLUEZ_H

#include <alsa/asoundlib.h>

extern void alsa_bluez_init();
extern int alsa_bluez_set_remote_device(const char * interface, const char * device, const char * profile);

#endif /* __INC_ALSA_BLUEZ_H */
