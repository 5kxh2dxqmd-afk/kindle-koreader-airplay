/*
 * dnssd_stub.c — stub dnssd implementation for Kindle.
 * UxPlay uses dnssd for Bonjour/Avahi registration and for supplying
 * device metadata to GET /info handlers.  On Kindle we have no Bonjour;
 * our own mDNS thread handles discovery.  This stub satisfies the API.
 *
 * IMPORTANT: dnssd.h already provides a complete definition of
 * `struct dnssd_s` (name/name_len, hw_addr/hw_addr_len, pk, features1,
 * features2, pin_pw, dnssd_private) — it is NOT an opaque forward
 * declaration. We must NOT redefine that struct here. Anything we need
 * that isn't part of the public struct (formatted hw-addr string,
 * formatted pk string, the prebuilt TXT record blob) lives in a private
 * struct hung off d->dnssd_private.
 */
#include <stddef.h>
#include <stdint.h>
#include "dnssd.h"
#include "dnssdint.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#define MAX_DEVICEID 18

typedef struct {
    char          hw_addr_str[MAX_DEVICEID]; /* "AA:BB:CC:DD:EE:FF" */
    char          pk_str[128];               /* base64 Ed25519 pk */
    unsigned char airplay_txt[512];          /* prebuilt TXT record blob */
    int           airplay_txt_len;
} dnssd_priv_t;

static uint64_t get_features(dnssd_t *d)
{
    return ((uint64_t)d->features2 << 32) | (uint64_t)d->features1;
}

static void set_features(dnssd_t *d, uint64_t f)
{
    d->features1 = (uint32_t)(f & 0xFFFFFFFFULL);
    d->features2 = (uint32_t)(f >> 32);
}

static void build_airplay_txt(dnssd_t *d)
{
    dnssd_priv_t *priv = (dnssd_priv_t *)d->dnssd_private;
    char feat[32], flags[16], pk_entry[150], model[32], srcvers[24], vv[8];
    snprintf(feat,    sizeof(feat),    "features=0x%llx,0x1E",
             (unsigned long long)(get_features(d) & 0xFFFFFFFFULL));
    snprintf(flags,   sizeof(flags),   "flags=0x4");
    snprintf(pk_entry,sizeof(pk_entry),"pk=%s", priv->pk_str);
    snprintf(model,   sizeof(model),   "model=AppleTV3,2");
    snprintf(srcvers, sizeof(srcvers), "srcvers=220.68");
    snprintf(vv,      sizeof(vv),      "vv=2");

    char deviceid[32];
    snprintf(deviceid, sizeof(deviceid), "deviceid=%s", priv->hw_addr_str);

    const char *parts[] = { deviceid, feat, flags, model, pk_entry, srcvers, vv, NULL };
    unsigned char *p = priv->airplay_txt;
    for (int i = 0; parts[i]; i++) {
        uint8_t len = (uint8_t)strlen(parts[i]);
        *p++ = len;
        memcpy(p, parts[i], len); p += len;
    }
    priv->airplay_txt_len = (int)(p - priv->airplay_txt);
}

dnssd_t *dnssd_init(const char *name, int name_len,
                     const char *hw_addr, int hw_addr_len,
                     unsigned char pin_pw, int *error)
{
    dnssd_t *d = calloc(1, sizeof(*d));
    if (!d) { if (error) *error = DNSSD_ERROR_OUTOFMEM; return NULL; }

    dnssd_priv_t *priv = calloc(1, sizeof(*priv));
    if (!priv) { free(d); if (error) *error = DNSSD_ERROR_OUTOFMEM; return NULL; }
    d->dnssd_private = priv;

    int nl = name_len > 0 ? name_len : 0;
    d->name = malloc((size_t)nl + 1);
    if (!d->name) { free(priv); free(d); if (error) *error = DNSSD_ERROR_OUTOFMEM; return NULL; }
    memcpy(d->name, name, (size_t)nl);
    d->name[nl] = '\0';
    d->name_len = nl;

    int hl = hw_addr_len < 6 ? hw_addr_len : 6;
    d->hw_addr = malloc(6);
    if (!d->hw_addr) { free(d->name); free(priv); free(d); if (error) *error = DNSSD_ERROR_OUTOFMEM; return NULL; }
    memset(d->hw_addr, 0, 6);
    memcpy(d->hw_addr, hw_addr, (size_t)hl);
    d->hw_addr_len = 6;

    utils_hwaddr_airplay(priv->hw_addr_str, sizeof(priv->hw_addr_str), d->hw_addr, d->hw_addr_len);

    d->pin_pw = pin_pw;
    d->pk = NULL;
    priv->pk_str[0] = '\0';

    /* default features = RPiPlay/UxPlay mirroring bits */
    set_features(d, 0x5A7FFFF7ULL);

    build_airplay_txt(d);
    if (error) *error = DNSSD_ERROR_NOERROR;
    return d;
}

int dnssd_register_raop(dnssd_t *d, unsigned short port)    { (void)d;(void)port; return 0; }
int dnssd_register_airplay(dnssd_t *d, unsigned short port) { (void)d;(void)port; return 0; }
void dnssd_unregister_raop(dnssd_t *d)    { (void)d; }
void dnssd_unregister_airplay(dnssd_t *d) { (void)d; }

const char *dnssd_get_airplay_txt(dnssd_t *d, int *length)
{
    dnssd_priv_t *priv = (dnssd_priv_t *)d->dnssd_private;
    if (length) *length = priv->airplay_txt_len;
    return (const char *)priv->airplay_txt;
}

const char *dnssd_get_raop_txt(dnssd_t *d, int *length)
{
    /* We don't do audio; return an empty TXT record. */
    if (length) *length = 0;
    (void)d;
    return "";
}

const char *dnssd_get_name(dnssd_t *d, int *length)
{
    if (length) *length = d->name_len;
    return d->name;
}

const char *dnssd_get_hw_addr(dnssd_t *d, int *length)
{
    if (length) *length = d->hw_addr_len;
    return d->hw_addr;
}

void dnssd_set_airplay_features(dnssd_t *d, int bit, int val)
{
    uint64_t f = get_features(d);
    if (val) f |=  (1ULL << bit);
    else     f &= ~(1ULL << bit);
    set_features(d, f);
    build_airplay_txt(d);
}

uint64_t dnssd_get_airplay_features(dnssd_t *d) { return get_features(d); }

void dnssd_set_pk(dnssd_t *d, char *pk_str)
{
    if (!pk_str) return;
    dnssd_priv_t *priv = (dnssd_priv_t *)d->dnssd_private;
    strncpy(priv->pk_str, pk_str, sizeof(priv->pk_str) - 1);
    priv->pk_str[sizeof(priv->pk_str) - 1] = '\0';

    free(d->pk);
    d->pk = strdup(pk_str);

    build_airplay_txt(d);
}

void dnssd_destroy(dnssd_t *d)
{
    if (!d) return;
    free(d->dnssd_private);
    free(d->name);
    free(d->hw_addr);
    free(d->pk);
    free(d);
}
