#include "../mm/vmem64.h"
#include "../k64_console.h"

nq_u64 vmem64_phys_to_direct(nq_u64 phys) {
    return VMEM64_DIRECT_MAP_BASE + phys;
}

nq_u64 vmem64_direct_to_phys(nq_u64 virt) {
    return virt - VMEM64_DIRECT_MAP_BASE;
}

void* vmem64_phys_to_ptr(nq_u64 phys) {
    return (void*)vmem64_phys_to_direct(phys);
}

nq_u64 vmem64_ptr_to_phys(const void* ptr) {
    nq_u64 virt = (nq_u64)ptr;

    if (vmem64_is_direct(virt)) {
        return vmem64_direct_to_phys(virt);
    }

    /*
     * Temporary compatibility:
     * low identity pointers still equal physical addresses.
     */
    if (vmem64_is_identity(virt)) {
        return virt;
    }

    return 0;
}

int vmem64_is_direct(nq_u64 virt) {
    return virt >= VMEM64_DIRECT_MAP_BASE &&
           virt <  VMEM64_DIRECT_MAP_BASE + VMEM64_DIRECT_MAP_SIZE;
}

int vmem64_is_user(nq_u64 virt) {
    return virt >= VMEM64_USER_BASE &&
           virt <  VMEM64_USER_LIMIT;
}

int vmem64_is_identity(nq_u64 virt) {
    return virt >= VMEM64_IDENTITY_BASE &&
           virt <  VMEM64_IDENTITY_BASE + VMEM64_IDENTITY_SIZE;
}

void vmem64_print_layout(void) {
    k64_print("VMEM64 layout:\n");

    k64_print("  user:       ");
    k64_print_hex64(VMEM64_USER_BASE);
    k64_print(" - ");
    k64_print_hex64(VMEM64_USER_LIMIT);
    k64_print("\n");

    k64_print("  identity:   ");
    k64_print_hex64(VMEM64_IDENTITY_BASE);
    k64_print(" - ");
    k64_print_hex64(VMEM64_IDENTITY_BASE + VMEM64_IDENTITY_SIZE);
    k64_print(" temporary\n");

    k64_print("  direct map: ");
    k64_print_hex64(VMEM64_DIRECT_MAP_BASE);
    k64_print(" - ");
    k64_print_hex64(VMEM64_DIRECT_MAP_BASE + VMEM64_DIRECT_MAP_SIZE);
    k64_print("\n");

    k64_print("  kernel:     ");
    k64_print_hex64(VMEM64_KERNEL_BASE);
    k64_print("\n");

    k64_print("  heap:       ");
    k64_print_hex64(VMEM64_KERNEL_HEAP_BASE);
    k64_print(" - ");
    k64_print_hex64(VMEM64_KERNEL_HEAP_BASE + VMEM64_KERNEL_HEAP_SIZE);
    k64_print(" future\n");
}