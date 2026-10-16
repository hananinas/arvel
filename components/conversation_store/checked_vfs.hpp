#pragma once
#include <cstddef>
namespace conversation_store::internal {
int configure_vfs(const char *path);
bool random(void *out, size_t bytes);
#ifdef CONVERSATION_STORE_HOST
void set_fault(int kind, int after);
#endif
}
