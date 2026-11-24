/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * wnk_keystore: a VLC keystore (saved sign-ins) whose storage is the application's. VLC keeps the
 * entries in memory; after each change the module hands the application the whole set as one text
 * (save), and reads it back when VLC opens the keystore (load). The application decides where it
 * goes (VPlayer: the kit's secret store, sealed by the TV's keymanager3 where the firmware has it).
 *
 * The application passes the address of a wnk_keystore_io to libVLC as
 * "--wnk-keystore=0x<address>" and selects the module with "--keystore=wnk_keystore". Without the
 * option the module does not open, and VLC falls back to its other keystores.
 *
 * The text: one line per entry, eight tab-separated fields: the seven VLC key values (protocol, user,
 * server, path, port, realm, authtype), each base64 or "-" when unset, then the secret in base64.
 */
#ifndef WNK_KEYSTORE_H
#define WNK_KEYSTORE_H

#define WNK_KEYSTORE_ABI 1

typedef struct wnk_keystore_io {
    int abi;      /* WNK_KEYSTORE_ABI */
    void *opaque;
    /* The saved text, malloc'd (the module frees it), or NULL when there is none. */
    char *(*load)(void *opaque);
    /* Keeps `text` (the whole set). 0 on success. Called with VLC's keystore lock held. */
    int (*save)(void *opaque, const char *text);
} wnk_keystore_io;

#endif
