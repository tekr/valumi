/*
 * Panel authentication on the device: auth_core plus a mutex and the
 * hardware RNG. Callable from any task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "auth_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Try a password login.
 * @return 1 ok (session id in @p sid), 0 wrong password, -1 locked out.
 */
int auth_login_password(const char *password, char sid[AUTH_TOKEN_HEX + 1]);

/** Spend a QR token for a new session. */
bool auth_login_qr(const char *token, char sid[AUTH_TOKEN_HEX + 1]);

bool auth_check(const char *sid);
void auth_logout(const char *sid);

/** Issue the token for a QR code about to be shown for @p screen_s seconds.
 * It stays redeemable a little longer (AUTH_QR_GRACE_S) for the phone to
 * load the link, and no longer. */
void auth_issue_qr(int screen_s, char token[AUTH_TOKEN_HEX + 1]);

#define AUTH_QR_GRACE_S 15

#ifdef __cplusplus
}
#endif
