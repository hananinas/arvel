#ifndef ESPEAK_EMBEDDED_WORKSPACE_H
#define ESPEAK_EMBEDDED_WORKSPACE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *espeak_workspace_alloc(size_t size);
int espeak_translation_workspace_init(void);
int espeak_phoneme_workspace_init(void);
int espeak_pitch_workspace_init(void);
int espeak_clause_workspace_init(void);
#ifdef __cplusplus
}
#endif
#endif
