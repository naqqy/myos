#include "usermode64.h"

#include "../../k64_console.h"
#include "../../mm/kmem64.h"
#include "../../mm/paging64.h"

#define USERMODE64_TEST_STACK_SIZE (16u * 1024u)

extern void usermode64_enter(nq_u64 user_rip, nq_u64 user_rsp);
extern void usermode64_test_entry(void);

static void* g_user_test_stack = 0;

void usermode64_enter_test(void) {
    nq_u64 stack_top;
    nq_u64 user_rip;

    if (!g_user_test_stack) {
        g_user_test_stack = kmalloc64(USERMODE64_TEST_STACK_SIZE);

        if (!g_user_test_stack) {
            k64_print("usermode64: failed to allocate test stack\n");
            return;
        }
    }

    stack_top =
        (nq_u64)g_user_test_stack + USERMODE64_TEST_STACK_SIZE;

    stack_top &= ~0xFull;

    user_rip = (nq_u64)usermode64_test_entry;

    k64_print("Entering ring 3 test...\n");

    k64_print("User RIP: ");
    k64_print_hex64(user_rip);
    k64_print("\n");

    k64_print("User stack base: ");
    k64_print_hex64((nq_u64)g_user_test_stack);
    k64_print("\n");

    k64_print("User RSP: ");
    k64_print_hex64(stack_top);
    k64_print("\n");

    /*
     * Ring 3 can only execute/read/write pages where the USER bit is set
     * at every paging level.
     *
     * For this first proof-of-concept, usermode64_test_entry still lives
     * inside the kernel image. So we temporarily mark that containing
     * identity-mapped 2 MiB page as user-accessible.
     *
     * This is NOT the final security model. Later, real user programs
     * should live in separate user pages, not inside kernel text.
     */
    if (!paging64_mark_user_range(user_rip, 4096)) {
        k64_print("usermode64: failed to mark user code page\n");
        return;
    }

    /*
     * The test stack is allocated by kmalloc64, so its backing physical
     * pages are currently identity-mapped as supervisor pages. Mark them
     * user-accessible for this ring 3 test.
     */
    if (!paging64_mark_user_range(
            (nq_u64)g_user_test_stack,
            USERMODE64_TEST_STACK_SIZE)) {
        k64_print("usermode64: failed to mark user stack pages\n");
        return;
    }

    k64_print("User pages marked accessible.\n");

    /*
     * This does not return unless the user code faults and your exception
     * handler eventually recovers. For now, expect either:
     *
     *   - infinite loop in ring 3, or
     *   - a user-mode exception with CS = 0x23 if test code triggers ud2.
     */
    usermode64_enter(user_rip, stack_top);
}