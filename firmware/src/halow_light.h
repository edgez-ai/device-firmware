#ifndef HALOW_LIGHT_H
#define HALOW_LIGHT_H

#include <stdbool.h>

#ifdef RADAR_HALOW_LIGHT
int halow_light_start(bool initial_pressed);
void halow_light_button_changed(bool pressed);
void halow_light_radar_sample(bool present, float range_m);
void halow_light_link_changed(bool connected);
#else
static inline void halow_light_link_changed(bool connected) { (void)connected; }
#endif
#endif
