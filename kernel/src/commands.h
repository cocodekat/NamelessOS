#ifndef COMMANDS_H
#define COMMANDS_H

void cmd_help(void);
void cmd_echo(const char *args);
void cmd_fart(const char *args);
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
void cmd_sysinfo(const char *args);
void kb_readline(char *buf, int max_len);
void cmd_therapist(const char *args);

void g_flappy_bird(void);

void cmd_execute(const char *name, const char *args);

#endif