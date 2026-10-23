#ifndef EMBK_SESSION_ABI_H
#define EMBK_SESSION_ABI_H
#define EMBK_SESSION_STATUS 0
#define EMBK_SESSION_LOCK 1
#define EMBK_SESSION_LOGOUT 2
/* Only the init process named by the kernel can manage authentication. */
#define EMBK_SESSION_BEGIN 10
#define EMBK_SESSION_GREETER 11
#define EMBK_SESSION_UNLOCK 12
#define EMBK_SESSION_END 13
#define EMBK_SESSION_LOCKED 1
#define EMBK_SESSION_ENDING 2
#define EMBK_SESSION_ACTIVE 4
#define EMBK_SESSION_LIVE 8
#endif
