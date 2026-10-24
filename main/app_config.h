#pragma once

/* Names several modules must agree on */

/* How phones list the star: DLNA renderer, AirPlay and Spotify Connect device. Used in string
 * literals and as is in the DLNA description XML: no XML special characters. The web page
 * names it in its hints too (index.html). */
#define DEVICE_NAME     "Новогодняя звезда"

/* NVS namespace of the settings, the home network and the access point password; the factory
 * reset erases all of it */
#define NVS_NAMESPACE   "star"
