/* PearPC -- limited CHRP boot-script recognition (GPL-2.0-or-later). */
#ifndef PEARPC_CHRPBOOT_H
#define PEARPC_CHRPBOOT_H

#include <string>

#include <string>

// Recognize direct boot/$boot commands and ybin's bootyaboot entry point.
// This is not a Forth interpreter: it does not evaluate menus or conditions.
bool prom_parse_chrp_boot_script(const char *script, std::string &path, std::string &args);

#endif
