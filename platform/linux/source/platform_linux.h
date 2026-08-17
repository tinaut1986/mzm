#ifndef PLATFORM_LINUX_H
#define PLATFORM_LINUX_H

#include "types.h"
#include <stdbool.h>

void Platform_Linux_Init(bool headless);
void Platform_Linux_Cleanup(void);
void Platform_Linux_VBlank(void);
u16  Platform_Linux_PollKeys(void);
bool Platform_Linux_ShouldQuit(void);

#endif /* PLATFORM_LINUX_H */
