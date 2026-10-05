/*
 * The short codes the ticker makes up and shows on its screen: the panel
 * password and the setup hotspot's passphrase. Upper case and digits only,
 * because the screen font has no lower case, and no 0/O or 1/I, because
 * people copy them off a small screen.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @p len random characters into @p out (which holds len + 1). */
void code_random(char *out, int len, uint32_t (*rnd)(void));

/** Exactly @p len characters, all from the alphabet. */
bool code_valid(const char *s, int len);

#ifdef __cplusplus
}
#endif
