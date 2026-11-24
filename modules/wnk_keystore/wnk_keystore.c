/* SPDX-License-Identifier: GPL-3.0-or-later
 * wnk_keystore: VLC's saved sign-ins in the application's storage (wnk_keystore.h). Built on VLC's
 * memory keystore (modules/keystore/memory.c), with a load at open and a save after each change.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_keystore.h>
#include <vlc_plugin.h>
#include <vlc_strings.h>

#include "list_util.h"
#include "wnk_keystore.h"

static int Open(vlc_object_t *);
static void Close(vlc_object_t *);

vlc_module_begin()
    set_shortname("wnk_keystore")
    set_description("Saved sign-ins in the application's storage")
    set_category(CAT_ADVANCED)
    set_subcategory(SUBCAT_ADVANCED_MISC)
    add_string("wnk-keystore", NULL, "Storage callbacks", "Set by the application (wnk_keystore.h).", true)
        change_volatile()
    set_capability("keystore", 100)
    set_callbacks(Open, Close)
vlc_module_end()

struct vlc_keystore_sys {
    struct ks_list list;
    vlc_mutex_t lock;
    const wnk_keystore_io *io;
};

/* ---- text form ---- */

static char *encode_value(const char *v) {
    if (v == NULL) return strdup("-");
    return vlc_b64_encode_binary((const uint8_t *)v, strlen(v));
}

static int Save(vlc_keystore_sys *sys) {
    size_t cap = 256, len = 0;
    char *text = malloc(cap);
    if (text == NULL) return VLC_ENOMEM;
    text[0] = '\0';
    for (unsigned i = 0; i < sys->list.i_count; ++i) {
        const vlc_keystore_entry *e = &sys->list.p_entries[i];
        if (e->p_secret == NULL) continue;  /* released (removed) */
        char *fields[KEY_MAX + 1] = {0};
        size_t need = 0;
        int ok = 1;
        for (unsigned k = 0; k < KEY_MAX; ++k) {
            fields[k] = encode_value(e->ppsz_values[k]);
            ok = ok && fields[k] != NULL;
        }
        fields[KEY_MAX] = vlc_b64_encode_binary(e->p_secret, e->i_secret_len);
        ok = ok && fields[KEY_MAX] != NULL;
        for (unsigned k = 0; ok && k <= KEY_MAX; ++k) need += strlen(fields[k]) + 1;
        if (ok && len + need + 1 > cap) {
            while (len + need + 1 > cap) cap *= 2;
            char *grown = realloc(text, cap);
            if (grown == NULL) ok = 0; else text = grown;
        }
        for (unsigned k = 0; ok && k <= KEY_MAX; ++k) {
            const size_t n = strlen(fields[k]);
            memcpy(text + len, fields[k], n);
            len += n;
            text[len++] = k == KEY_MAX ? '\n' : '\t';
        }
        text[len] = '\0';
        for (unsigned k = 0; k <= KEY_MAX; ++k) free(fields[k]);
        if (!ok) {
            free(text);
            return VLC_ENOMEM;
        }
    }
    const int r = sys->io->save(sys->io->opaque, text);
    free(text);
    return r == 0 ? VLC_SUCCESS : VLC_EGENERIC;
}

static char *decode_value(const char *field) {
    if (strcmp(field, "-") == 0) return NULL;
    uint8_t *out = NULL;
    const size_t n = vlc_b64_decode_binary(&out, field);
    if (out == NULL) return NULL;
    char *s = malloc(n + 1);
    if (s != NULL) {
        memcpy(s, out, n);
        s[n] = '\0';
    }
    free(out);
    return s;
}

static void Load(vlc_keystore_sys *sys) {
    char *text = sys->io->load(sys->io->opaque);
    if (text == NULL) return;
    char *save_line = NULL;
    for (char *line = strtok_r(text, "\n", &save_line); line != NULL; line = strtok_r(NULL, "\n", &save_line)) {
        char *fields[KEY_MAX + 1];
        unsigned n = 0;
        char *save_field = NULL;
        for (char *f = strtok_r(line, "\t", &save_field); f != NULL && n <= KEY_MAX; f = strtok_r(NULL, "\t", &save_field))
            fields[n++] = f;
        if (n != KEY_MAX + 1) continue;  /* not ours: skipped */
        vlc_keystore_entry *e = ks_list_new_entry(&sys->list);
        if (e == NULL) break;
        for (unsigned k = 0; k < KEY_MAX; ++k) e->ppsz_values[k] = decode_value(fields[k]);
        uint8_t *secret = NULL;
        const size_t len = vlc_b64_decode_binary(&secret, fields[KEY_MAX]);
        e->p_secret = secret;
        e->i_secret_len = secret != NULL ? len : 0;
        if (e->p_secret == NULL || e->ppsz_values[KEY_PROTOCOL] == NULL || e->ppsz_values[KEY_SERVER] == NULL) {
            vlc_keystore_release_entry(e);
            sys->list.i_count--;  /* the entry just added */
        }
    }
    free(text);
}

/* ---- keystore ops (as modules/keystore/memory.c, then saved) ---- */

static int Store(vlc_keystore *ks, const char *const values[KEY_MAX], const uint8_t *secret, size_t secret_len,
                 const char *label) {
    (void)label;
    vlc_keystore_sys *sys = ks->p_sys;
    int ret = VLC_EGENERIC;
    vlc_mutex_lock(&sys->lock);
    vlc_keystore_entry *e = ks_list_find_entry(&sys->list, values, NULL);
    if (e != NULL) {
        vlc_keystore_release_entry(e);
    } else {
        e = ks_list_new_entry(&sys->list);
    }
    if (e != NULL && ks_values_copy((const char **)e->ppsz_values, values) == 0 &&
        vlc_keystore_entry_set_secret(e, secret, secret_len) == 0)
        ret = Save(sys);
    vlc_mutex_unlock(&sys->lock);
    return ret;
}

static unsigned int Find(vlc_keystore *ks, const char *const values[KEY_MAX], vlc_keystore_entry **pp_entries) {
    vlc_keystore_sys *sys = ks->p_sys;
    struct ks_list out = {0};
    vlc_keystore_entry *e;
    unsigned index = 0;
    vlc_mutex_lock(&sys->lock);
    while ((e = ks_list_find_entry(&sys->list, values, &index)) != NULL) {
        vlc_keystore_entry *o = ks_list_new_entry(&out);
        if (o == NULL || ks_values_copy((const char **)o->ppsz_values, (const char *const *)e->ppsz_values) ||
            vlc_keystore_entry_set_secret(o, e->p_secret, e->i_secret_len)) {
            ks_list_free(&out);
            break;
        }
    }
    vlc_mutex_unlock(&sys->lock);
    *pp_entries = out.p_entries;
    return out.i_count;
}

static unsigned int Remove(vlc_keystore *ks, const char *const values[KEY_MAX]) {
    vlc_keystore_sys *sys = ks->p_sys;
    vlc_keystore_entry *e;
    unsigned index = 0, count = 0;
    vlc_mutex_lock(&sys->lock);
    while ((e = ks_list_find_entry(&sys->list, values, &index)) != NULL) {
        vlc_keystore_release_entry(e);
        count++;
    }
    if (count > 0) Save(sys);
    vlc_mutex_unlock(&sys->lock);
    return count;
}

static int Open(vlc_object_t *obj) {
    vlc_keystore *ks = (vlc_keystore *)obj;
    char *addr = var_InheritString(obj, "wnk-keystore");
    if (addr == NULL) return VLC_EGENERIC;
    void *p = NULL;
    const int scanned = sscanf(addr, "%p", &p);
    free(addr);
    const wnk_keystore_io *io = p;
    if (scanned != 1 || io == NULL || io->abi != WNK_KEYSTORE_ABI || io->load == NULL || io->save == NULL) {
        msg_Err(obj, "no storage callbacks from the application");
        return VLC_EGENERIC;
    }
    vlc_keystore_sys *sys = calloc(1, sizeof(*sys));
    if (sys == NULL) return VLC_ENOMEM;
    sys->io = io;
    vlc_mutex_init(&sys->lock);
    Load(sys);
    msg_Dbg(obj, "%u saved sign-in(s)", sys->list.i_count);
    ks->p_sys = sys;
    ks->pf_store = Store;
    ks->pf_find = Find;
    ks->pf_remove = Remove;
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj) {
    vlc_keystore *ks = (vlc_keystore *)obj;
    vlc_keystore_sys *sys = ks->p_sys;
    ks_list_free(&sys->list);
    vlc_mutex_destroy(&sys->lock);
    free(sys);
}
