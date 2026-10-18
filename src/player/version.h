/* src/player/version.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_VERSION_H
#define AUDIOLAZ_VERSION_H
#ifndef ROOMD_VERSION
#define ROOMD_VERSION "dev"
#endif
#ifdef ROOM_EN
#define ROOMD_LANG "en"
#else
#define ROOMD_LANG "fr"
#endif
#define ROOMD_ID "audiolaz-roomd " ROOMD_VERSION " " ROOMD_LANG
#endif
