/*
 * Free-memory near misses, logged where they can be traced.
 *
 * The heap's low-water mark says how close the ticker came to running out,
 * not when or why. Callers note what they have just done; the first to see a
 * new low under MEMWATCH_WARN_BYTES logs it, so the culprit is whatever ran
 * since the previous note.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define MEMWATCH_WARN_BYTES (16 * 1024)

/** Any task except the render task, which must not log. */
void memwatch_note(const char *what, const char *detail);

#ifdef __cplusplus
}
#endif
