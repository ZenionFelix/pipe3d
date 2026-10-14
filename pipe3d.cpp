#include <android/log.h>
__attribute__((constructor))
static void init() {
  __android_log_print(ANDROID_LOG_INFO, "pipe3d", "pipe3d loaded");
}
