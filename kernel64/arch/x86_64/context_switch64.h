#ifndef CONTEXT_SWITCH64_H
#define CONTEXT_SWITCH64_H

#include "naq_boot_info.h"

void context_switch64(nq_u64* old_rsp, nq_u64 new_rsp);

#endif