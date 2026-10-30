/* g2-arm64: the loader's input files from a game folder (the user's own plaintext dump), made
 * the way the port's scripts/prepare.py, link_modules.py and content_profile.py make them on a
 * PC - for the Android app, which has no Python. */
#ifndef GAME_PREPARE_H
#define GAME_PREPARE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* <out_dir>/boot-linked.bin (BBPROBE5: <game_dir>/<exe> linked with the game's bundled
 * sce_module/libc.prx and libSceFios2.prx) and <out_dir>/content.bin (BBCONT01, SKU full, from
 * sce_sys/param.sfo). <out_dir> must exist. 0 on success; otherwise -1 and a message in error.
 * game_dir may be "<update folder>\n<game folder>" (issue #10: the game's update as a folder of
 * its own): a file is taken from the update folder when it has it, from the game's otherwise. */
int game_prepare(const char *game_dir, const char *exe, const char *out_dir, char *error, size_t error_size);

/* A string or number of <game_dir>/sce_sys/param.sfo as text (TITLE, TITLE_ID, APP_VER...):
 * 0 on success, -1 if the file or the key is missing. game_dir as for game_prepare. */
int game_sfo_value(const char *game_dir, const char *key, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
