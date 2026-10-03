/*
 * kbdlayout.c — keyboard layouts (see kbdlayout.h)
 */

#include "kbdlayout.h"
#include "../ke/printf.h"
#include "../lib/string.h"
#include "../um/um.h"
#include "../../userland/include/kbdlayouts.h"

#define PRELOAD_KEY "User\\" UM_USER_SID "\\Keyboard Layout\\Preload"

static int    g_cur;                   /* the layout in effect */
static UINT32 g_gen = ~0u;             /* the registry as it was when Preload was last read */

int         KbdCount(void)    { return KBD_NLAYOUTS; }
const char *KbdName(int i)    { return i >= 0 && i < KBD_NLAYOUTS ? kbd_layouts[i].name : ""; }
const char *KbdKlid(int i)    { return i >= 0 && i < KBD_NLAYOUTS ? kbd_layouts[i].klid : ""; }
UINT32      KbdHkl(int i)     { return i >= 0 && i < KBD_NLAYOUTS ? kbd_layouts[i].hkl : 0; }

static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

int KbdFind(const char *klid)
{
    for (int i = 0; i < KBD_NLAYOUTS; i++) {
        int k = 0;
        while (klid[k] && kbd_layouts[i].klid[k] && upper(klid[k]) == kbd_layouts[i].klid[k]) k++;
        if (!klid[k] && !kbd_layouts[i].klid[k]) return i;
    }
    return -1;
}

static void use(int i)
{
    if (i < 0) i = 0;
    if (i != g_cur) kprintf("[KBD] Keyboard layout %s (%s)\n", kbd_layouts[i].klid, kbd_layouts[i].name);
    g_cur = i;
    UmSharedKeyboard(kbd_layouts[i].hkl);
}

void KbdLoad(void)
{
    char klid[16];
    g_gen = um_registry_generation();
    use(um_registry_get_sz(PRELOAD_KEY, "1", klid, sizeof(klid)) ? KbdFind(klid) : 0);
}

int KbdCurrent(void)
{
    if (um_registry_generation() != g_gen) KbdLoad();      /* a program may have changed Preload */
    return g_cur;
}

void KbdSet(int i)
{
    if (i < 0 || i >= KBD_NLAYOUTS) return;
    um_registry_set_sz(PRELOAD_KEY, "1", kbd_layouts[i].klid);
    um_registry_set_sz(UM_SETUP_KEY, "KeyboardLayout", kbd_layouts[i].klid);
    g_cur = -1;
    KbdLoad();
}

bool KbdHasAltGr(void) { return kbd_layouts[KbdCurrent()].altgr != 0; }

static const KbdKey *key_of(UINT8 sc)
{
    const KbdLayout *l = &kbd_layouts[KbdCurrent()];
    for (int i = 0; i < l->nkeys; i++)
        if (l->keys[i].sc == sc) return &l->keys[i];
    return NULL;
}

UINT8 KbdVk(UINT8 sc)
{
    const KbdKey *k = key_of(sc);
    return k ? k->vk : 0;
}

bool KbdKeyChar(UINT8 sc, int level, bool caps, UINT16 *ch, bool *dead)
{
    const KbdKey *k = key_of(sc);
    if (!k) return false;
    if (caps && (k->flags & KBD_CAPS) && level < 2) level ^= 1;
    *ch = k->ch[level & 3];
    *dead = (k->flags & KBD_DEAD(level & 3)) != 0;
    return true;
}

UINT16 KbdComposeChar(UINT16 dead, UINT16 base)
{
    for (int i = 0; i < KBD_NCOMPOSE; i++)
        if (kbd_compose[i].dead == dead && kbd_compose[i].base == base) return kbd_compose[i].out;
    return 0;
}

bool KbdDecompose(UINT16 c, UINT16 *base, UINT16 *accent)
{
    for (int i = 0; i < KBD_NCOMPOSE; i++)
        if (kbd_compose[i].out == c && kbd_compose[i].base != ' ') {
            *base = kbd_compose[i].base;
            *accent = kbd_compose[i].dead;
            return true;
        }
    return false;
}
