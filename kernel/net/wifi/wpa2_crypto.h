#ifndef EMBLINK_WPA2_CRYPTO_H
#define EMBLINK_WPA2_CRYPTO_H
#include <stdint.h>
/* WPA2-Personal/CCMP security primitives. Adapter transport and association
 * are separate work; this API never reports that a network is connected. */
int wifi_wpa2_psk(const uint8_t *password,unsigned length,const uint8_t *ssid,unsigned ssid_length,uint8_t pmk[32]);
void wifi_wpa2_ptk(const uint8_t pmk[32],const uint8_t sta[6],const uint8_t ap[6],
    const uint8_t snonce[32],const uint8_t anonce[32],uint8_t ptk[48]);
/* Caller zeroes the EAPOL MIC field and supplies the exact frame length. */
void wifi_wpa2_mic(const uint8_t kck[16],const uint8_t *packet,unsigned length,uint8_t mic[16]);
int wifi_wpa2_unwrap(const uint8_t kek[16],const uint8_t *wrapped,unsigned length,uint8_t *out,unsigned capacity);
/* RSN IE body (excluding ID/length): require CCMP group/pairwise, PSK AKM and
 * no mandatory PMF, since management-frame protection is not implemented. */
int wifi_rsn_ccmp_psk(const uint8_t *body,unsigned length);
#endif
