/*
 * Confirming a new firmware image.
 *
 * Rollback is enabled: a new image boots on probation, and if it restarts
 * before confirming itself the bootloader returns to the previous one.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Confirm the running image if it is on probation. Call before any
 * deliberate restart, and before starting an update (which probation
 * forbids). */
void firmware_confirm(void);

/**
 * @brief Confirm once the panel has been reachable for a minute.
 *
 * Reachable, because that is what delivering the next update needs; for a
 * minute, because the first HTTPS requests run in the first seconds online,
 * and an image that crashes on them should still be the one rolled back.
 * Call every loop with whether the panel can be reached now.
 */
void firmware_confirm_when_settled(bool reachable, int64_t now_us);

#ifdef __cplusplus
}
#endif
