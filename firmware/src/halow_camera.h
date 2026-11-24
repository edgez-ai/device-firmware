#ifndef HALOW_CAMERA_H
#define HALOW_CAMERA_H

#include "liblwm2m.h"

extern lwm2m_object_t halow_camera;
int halow_camera_start(void);
void halow_camera_free(void *value);

#endif
