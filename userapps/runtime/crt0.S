.intel_syntax noprefix

.global _start
.extern main
.extern exit

.section .text

_start:
    /*
     * For now:
     *   argc = 0
     *   argv = 0
     * Later, the ELF loader can build a real user stack.
     */
    xor rdi, rdi
    xor rsi, rsi

    call main

    /*
     * main() return value becomes exit code.
     */
    mov rdi, rax
    call exit

.hang:
    hlt
    jmp .hang