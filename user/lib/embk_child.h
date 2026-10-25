#ifndef EMBK_CHILD_H
#define EMBK_CHILD_H
#include <stdint.h>
/* Native child authority stays in the caller's handle table. No PID grants
 * authority: PID waits resolve only handles the caller already owns. */
#define EMBK_CHILD_INFO 0u
#define EMBK_CHILD_WAIT_HANDLE 1u
#define EMBK_CHILD_WAIT_PID 2u
#define EMBK_CHILD_WAIT_STATUS 3u /* PID return + POSIX wait status copyout */
#define EMBK_CHILD_NOHANG 1u
#define EMBK_CHILD_ANY (-1)
#define EMBK_CHILD_DONE 1u
#define EMBK_CHILD_KILLED 2u
#define EMBK_CHILD_RETAINED 4u
struct embk_child_result {
    uint32_t pid;
    int32_t handle;
    int32_t exit_code;
    uint32_t flags;
};
#endif
