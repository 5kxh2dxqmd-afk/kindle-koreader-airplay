/*
 * dnssd_stub.c — stub dnssd implementation for Kindle.
 * UxPlay uses dnssd for Bonjour/Avahi registration and for supplying
 * device metadata to GET /info handlers.  On Kindle we have no Bonjour;
 * our own mDNS thread handles discovery.  This stub satisfies the API.
 */
#include <stddef.h>
#include "dnssd.h"
#include "dnssdint.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#define MAX_DEVICEID 18

struct dnssd_s {
    char  name[256];
    char  hw_addr[6];        /* raw 6-byte MAC */
    char  hw_addr_str[MAX_DEVICEID]; /* "AA:BB:CC:DD:EE:FF" */
    char  pk_str[128];       /* base64 Ed25519 pk, set by raop_set_dnssd */
    uint64_t features;

    /* prebuilt TXT record blobs (length-prefixed strings) */
    unsigned char airplay_txt[512];
    int           airplay_txt_len;
};

static void build_airplay_txt(dnssd_t *d)
{
    char feat[32], flags[16], pk_entry[150], model[32], srcvers[24], vv[8];
    snprintf(feat,    sizeof(feat),    "features=0x%llx,0x1E",
             (unsigned long long)(d->features & 0xFFFFFFFFULL));
    snprintf(flags,   sizeof(flags),   "flags=0x4");
    snprintf(pk_entry,sizeof(pk_entry),"pk=%s", d->pk_str);
    snprintf(model,   sizeof(model),   "model=AppleTV3,2");
    snprintf(srcvers, sizeof(srcvers), "srcvers=220.68");
    snprintf(vv,      sizeof(vv),      "vv=2");

    char deviceid[32];
    snprintf(deviceid, sizeof(deviceid), "deviceid=%s", d->hw_addr_str);

    const char *parts[] = { deviceid, feat, flags, model, pk_entry, srcvers, vv, NULL };
    unsigned char *p = d->airplay_txt;
    for (int i = 0; parts[i]; i++) {
        uint8_t len = (uint8_t)strlen(parts[i]);
        *p++ = len;
        memcpy(p, parts[i], len); p += len;
    }
    d->airplay_txt_len = (int)(p - d->airplay_txt);
}

dnssd_t *dnssd_init(const char *name, int name_len,
                    const char *hw_addr, int hw_addr_len,
                    int *error, unsigned char pin_pw)
{
    (void)pin_pw;
    dnssd_t *d = calloc(1, sizeof(*d));
    if (!d) { if (error) *error = 1; return NULL; }
    if (error) *error = 0;

    int nl = name_len < (int)sizeof(d->name) - 1 ? name_len : (int)sizeof(d->name) - 1;
    memcpy(d->name, name, nl);
    d->name[nl] = '\0';

    int hl = hw_addr_len < 6 ? hw_addr_len : 6;
    memcpy(d->hw_addr, hw_addr, hl);
    utils_hwaddr_airplay(d->hw_addr_str, sizeof(d->hw_addr_str), hw_addr, hl);

    /* default features = RPiPlay/UxPlay mirroring bits */
    d->features = 0x5A7FFFF7ULL;

    build_airplay_txt(d);
    return d;
}

int dnssd_register_raop(dnssd_t *d, unsigned short port)    { (void)d;(void)port; return 0; }
int dnssd_register_airplay(dnssd_t *d, unsigned short port) { (void)d;(void)port; return 0; }
void dnssd_unregister_raop(dnssd_t *d)    { (void)d; }
void dnssd_unregister_airplay(dnssd_t *d) { (void)d; }

const char *dnssd_get_airplay_txt(dnssd_t *d, int *length)
{
    if (length) *length = d->airplay_txt_len;
    return (const char *)d->airplay_txt;
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
    if (length) *length = (int)strlen(d->name);
    return d->name;
}

const char *dnssd_get_hw_addr(dnssd_t *d, int *hw_addr_len)
{
    if (hw_addr_len) *hw_addr_len = 6;
    return d->hw_addr;
}

void dnssd_set_airplay_features(dnssd_t *d, int bit, int val)
{
    if (val) d->features |=  (1ULL << bit);
    else     d->features &= ~(1ULL << bit);
    build_airplay_txt(d);
}

uint64_t dnssd_get_airplay_features(dnssd_t *d) { return d->features; }

void dnssd_set_pk(dnssd_t *d, char *pk_str)
{
    if (!pk_str) return;
    strncpy(d->pk_str, pk_str, sizeof(d->pk_str) - 1);
    build_airplay_txt(d);
}

void dnssd_destroy(dnssd_t *d) { free(d); }
