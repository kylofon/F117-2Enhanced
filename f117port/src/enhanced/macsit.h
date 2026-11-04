/* The Mac art read straight from the user's Mac release (Phase 7): --mac-art may name a classic StuffIt archive of the
 * Mac release (e.g. f117a_stealth_fighter.sit, nested archives too) or the "F117a Art" resource fork instead of a folder
 * made by tools/mac_art.py. Decoded in memory; nothing is written. */
#pragma once

#include "macart.h"

/* Decodes the CvPc pictures named in names[] (as tools/mac_art.py names its files) into out[] (zeroed by the caller;
 * filled entries own a malloc'd pix). Returns how many were found, -1 = the file can't be read. */
int enh_mac_sit_load(const char *path, const char *const *names, int count, MacPic *out);
