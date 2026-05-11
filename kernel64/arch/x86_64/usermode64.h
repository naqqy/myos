#ifndef USERMODE64_H
#define USERMODE64_H

#include "naq_boot_info.h"

void usermode64_enter_test(void);
void usermode64_enter(nq_u64 user_rip, nq_u64 user_rsp);

#endif