#ifndef EMBLINK_SYSTEM_STATUS_H
#define EMBLINK_SYSTEM_STATUS_H
#include "embk.h"
/* SSIDs are binary identifiers. Escape controls and non-ASCII bytes rather
 * than letting a nearby AP inject newlines or invalid UTF-8 into system UI. */
static inline void system_ssid(char out[129],const uint8_t *p,unsigned n) {
    static const char hex[]="0123456789abcdef";unsigned at=0;
    if(n>32) n=32;
    for(unsigned i=0;i<n;i++) {
        unsigned c=p[i];
        if(c>=32 && c<127 && c!='\\') out[at++]=c;
        else {out[at++]='\\';out[at++]='x';out[at++]=hex[c>>4];out[at++]=hex[c&15];}
    }
    out[at]=0;
}
static inline const char *system_wifi_status(const struct embk_wifi_info *s) {
    if(!(s->flags&EMBK_WIFI_READY)) {
        if(s->radio==1) return "Wi-Fi firmware is missing";
        if(s->radio==2) return "Wi-Fi firmware could not be loaded";
        if(s->radio==6) return "Wi-Fi service is unavailable";
        if(s->radio==7) return "Wi-Fi stopped; restart to try again";
        return "No supported Wi-Fi adapter";
    }
    if(s->flags&EMBK_WIFI_BLOCKED) return "Wi-Fi is blocked by the hardware switch";
    if(s->flags&EMBK_WIFI_BUSY) return "Saving connection changes…";
    if(s->flags&EMBK_WIFI_ADDRESS) return "Connected";
    if(s->flags&EMBK_WIFI_SCANNING) return "Looking for networks…";
    switch(s->state) {
    case 2:return "Authenticating…";
    case 3:return "Joining network…";
    case 4:return "Securing connection…";
    case 5:return "Waiting for a network address…";
    case 6:
        if(s->error==2) return "Network not found; trying again…";
        if(s->error==6) return "Security handshake failed; check the password";
        return "Connection lost; trying again…";
    default:return "Disconnected";
    }
}
static inline const char *system_request_error(int rc) {
    switch(-rc) {
    case 1:case 13:return "This change is not permitted.";
    case 2:return "That saved network could not be found.";
    case 5:return "The change could not be saved. Check the disk and try again.";
    case 16:return "Another operation is in progress. Please try again.";
    case 19:return "No supported device is available.";
    case 22:return "Check the network name and password.";
    case 28:return "Storage or the saved-network list is full. Remove an unused network.";
    default:return "The change could not be completed. Please try again.";
    }
}
#endif
