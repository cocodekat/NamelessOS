#ifndef COMMANDS_H
#define COMMANDS_H

void cmd_help(void);
void cmd_echo(const char *args);
void cmd_ls(void);
void cmd_cat(const char *filename);
void cmd_pwd(void);
void cmd_cd(const char *dir);
void cmd_mkdir(const char *name);
void cmd_mkfile(const char *name);
void cmd_rm(const char *name);
void cmd_write(const char *args);
void cmd_sync(void);
void cmd_edit(const char *filename);
void cmd_kbtest(void);
void kb_readline(char *buf, int max_len);
void serial_print_uint(unsigned int value);

#endif