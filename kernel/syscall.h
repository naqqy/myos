#ifndef SYSCALL_H
#define SYSCALL_H

typedef unsigned int u32;

#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_PUTCHAR         2
#define SYS_CLEAR           3
#define SYS_TICKS           4
#define SYS_SLEEP           5
#define SYS_POLL_KEY        6
#define SYS_READ_KEY        7
#define SYS_YIELD           8
#define SYS_GETPID          9
#define SYS_SET_RAW_INPUT   10
#define SYS_BEGIN_FRAME     11
#define SYS_END_FRAME       12
#define SYS_READ_FILE       13
#define SYS_MKDIR           14
#define SYS_RM              15
#define SYS_RMDIR           16


u32 syscall_handler_c(u32 num, u32 a, u32 b, u32 c, u32 d, u32 e);

#endif