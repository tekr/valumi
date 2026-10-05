/*
 * The text encoded in the ticker's QR codes. Pure, tested on the host.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief "WIFI:T:WPA;S:<ssid>;P:<password>;;" -- the de-facto format phone
 * cameras use to join a network. \ ; , : and " are backslash-escaped, as the
 * format requires. An empty password produces an open-network (T:nopass) code.
 *
 * @return false if it does not fit in @p n.
 */
bool wifi_qr_join_text(const char *ssid, const char *password, char *out, size_t n);

#ifdef __cplusplus
}
#endif
