#ifndef USERMODE_H
#define USERMODE_H

typedef unsigned int u32;

void usermode_enter(u32 entry, u32 user_stack);

#endif