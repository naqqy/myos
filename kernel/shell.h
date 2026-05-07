#ifndef SHELL_H
#define SHELL_H

void shell_init(void);
void shell_putc(char c);
void shell_poll(void);
void shell_cancel_app_read_line(void);
void shell_force_kernel_input(void);
void shell_pause_app_input_for_ker(void);
void shell_resume_app_input_after_fg(void);

#endif