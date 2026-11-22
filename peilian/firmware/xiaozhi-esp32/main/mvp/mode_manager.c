/* 模式状态机实现：仅维护当前模式变量，编排逻辑在 MvpApp。 */
#include "mvp/mode_manager.h"

static MvpMode s_mode = MODE_BADGE;

void ModeManager_Init(void) {
    s_mode = MODE_BADGE;
}

MvpMode ModeManager_GetMode(void) {
    return s_mode;
}

int ModeManager_Switch(MvpMode mode) {
    if (mode > MODE_SETTINGS) {
        return -1;
    }
    s_mode = mode;
    return 0;
}
