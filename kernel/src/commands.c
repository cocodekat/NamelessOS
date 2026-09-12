#include <stddef.h>
#include <limine.h>
#include "serial.h"
#include "tar.h"
#include "fs.h"
#include "drivers/keyboard.h"

#define EDIT_MAX_SIZE 8192
static char edit_buf[EDIT_MAX_SIZE];

#define KEY_CTRL_S 0x13
#define EDIT_HEADER_LINES 3 // title line + blank line before content starts

extern volatile struct limine_module_request module_request;

// Tracks the current directory, e.g. "" for root, "docs" if you cd into docs.
static char cwd[128] = "";

static int streq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static const void *get_archive(void)
{
    return fs_get_archive();
}

static void *get_archive_mutable(void)
{
    return fs_get_archive();
}
// Combines cwd + a relative name into one path, e.g. "docs" + "notes.txt" -> "docs/notes.txt"
static void resolve_path(char *out, size_t out_size, const char *name)
{
    if (cwd[0] == '\0')
    {
        // at root, just use the name as-is
        int i = 0;
        while (name[i] && i < (int)out_size - 1)
        {
            out[i] = name[i];
            i++;
        }
        out[i] = '\0';
    }
    else
    {
        int i = 0;
        while (cwd[i] && i < (int)out_size - 1)
        {
            out[i] = cwd[i];
            i++;
        }
        if (i < (int)out_size - 1)
            out[i++] = '/';
        int j = 0;
        while (name[j] && i < (int)out_size - 1)
        {
            out[i++] = name[j++];
        }
        out[i] = '\0';
    }
}

void cmd_help(void)
{
    serial_print("Available commands:\n");
    serial_print("  help  - show this message\n");
    serial_print("  echo  - print the rest of the line\n");
    serial_print("  clear - clear the screen\n");
    serial_print("  ls    - list files in current directory\n");
    serial_print("  cat   - print a file's contents\n");
    serial_print("  cd    - change directory\n");
    serial_print("  pwd   - print current directory\n");
    serial_print("  sync  - save filesystem changes to disk\n");
    serial_print("  edit  - edit a file (Ctrl+K to save)\n");
}

void cmd_echo(const char *args)
{
    serial_print(args);
    serial_print("\n");
}

static void ls_callback(const char *name, size_t size)
{
    (void)size;

    size_t cwd_len = 0;
    while (cwd[cwd_len])
        cwd_len++;

    if (cwd_len > 0)
    {
        for (size_t i = 0; i < cwd_len; i++)
        {
            if (name[i] != cwd[i])
                return;
        }
        if (name[cwd_len] != '/')
            return;
        name += cwd_len + 1;
    }

    if (name[0] == '\0')
        return;

    size_t len = 0;
    while (name[len])
        len++;

    int slash_index = -1;
    for (size_t i = 0; i < len; i++)
    {
        if (name[i] == '/')
        {
            slash_index = (int)i;
            break;
        }
    }

    // Skip anything nested deeper than one level (e.g. "docs/notes.txt" at root).
    // A directory itself is fine, since its slash is the very last character.
    if (slash_index != -1 && slash_index != (int)len - 1)
    {
        return;
    }

    serial_print(name);
    serial_print("\n");
}

void cmd_ls(void)
{
    const void *archive = get_archive();
    if (archive == NULL)
    {
        serial_print("No filesystem loaded.\n");
        return;
    }
    tar_list(archive, ls_callback);
}

void cmd_cat(const char *filename)
{
    const void *archive = get_archive();
    if (archive == NULL)
    {
        serial_print("No filesystem loaded.\n");
        return;
    }
    if (filename[0] == '\0')
    {
        serial_print("Usage: cat <filename>\n");
        return;
    }

    char full_path[128];
    resolve_path(full_path, sizeof(full_path), filename);

    size_t size;
    const void *data = tar_find(archive, full_path, &size);
    if (data == NULL)
    {
        serial_print("File not found: ");
        serial_print(full_path);
        serial_print("\n");
        return;
    }

    const char *bytes = (const char *)data;
    for (size_t i = 0; i < size; i++)
    {
        serial_putc(bytes[i]);
    }
    serial_print("\n");
}

void cmd_pwd(void)
{
    serial_print("/");
    serial_print(cwd);
    serial_print("\n");
}

void cmd_cd(const char *dir)
{
    if (dir[0] == '\0' || streq(dir, "/"))
    {
        cwd[0] = '\0';
        return;
    }
    if (streq(dir, ".."))
    {
        // Pop the last path segment off cwd.
        size_t len = 0;
        while (cwd[len])
            len++;
        while (len > 0 && cwd[len - 1] != '/')
            len--;
        if (len > 0)
            len--; // also remove the slash itself
        cwd[len] = '\0';
        return;
    }
    // Otherwise, append the new segment to cwd.
    char new_cwd[128];
    resolve_path(new_cwd, sizeof(new_cwd), dir);

    int i = 0;
    while (new_cwd[i] && i < (int)sizeof(cwd) - 1)
    {
        cwd[i] = new_cwd[i];
        i++;
    }
    cwd[i] = '\0';
}

void cmd_mkdir(const char *name)
{
    if (name[0] == '\0')
    {
        serial_print("Usage: mkdir <name>\n");
        return;
    }
    char full_path[128];
    resolve_path(full_path, sizeof(full_path), name);
    if (!tar_append_dir(get_archive_mutable(), fs_get_capacity(), full_path))
    {
        serial_print("mkdir failed (out of space?)\n");
    }
}

void cmd_mkfile(const char *name)
{
    if (name[0] == '\0')
    {
        serial_print("Usage: mkfile <name>\n");
        return;
    }
    char full_path[128];
    resolve_path(full_path, sizeof(full_path), name);
    if (!tar_append_file(get_archive_mutable(), fs_get_capacity(), full_path, NULL, 0))
    {
        serial_print("mkfile failed (out of space?)\n");
    }
}

void cmd_rm(const char *args)
{
    if (args[0] == '\0')
    {
        serial_print("Usage: rm [-rf] <name>\n");
        return;
    }

    int force = 0;
    const char *name = args;

    if (name[0] == '-' && name[1] == 'r' && name[2] == 'f' &&
        (name[3] == ' ' || name[3] == '\0'))
    {
        force = 1;
        name += 3;
        while (*name == ' ')
            name++;
    }

    if (name[0] == '\0')
    {
        serial_print("Usage: rm [-rf] <name>\n");
        return;
    }

    char full_path[128];
    resolve_path(full_path, sizeof(full_path), name);

    if (!force && tar_dir_has_children(get_archive(), full_path))
    {
        serial_print("Directory not empty (use rm -rf to force): ");
        serial_print(full_path);
        serial_print("\n");
        return;
    }

    if (force)
    {
        // Repeatedly remove any child entries until none remain, then remove the dir itself.
        while (tar_dir_has_children(get_archive(), full_path))
        {
            // Find one child's exact name to remove — reuse tar_remove's own
            // matching logic by scanning again from fs.c's mutable buffer.
            char child[128];
            if (!tar_first_child(get_archive(), full_path, child, sizeof(child)))
            {
                break; // shouldn't happen, but avoid infinite loop just in case
            }
            tar_remove(get_archive_mutable(), child);
        }
    }

    if (!tar_remove(get_archive_mutable(), full_path))
    {
        serial_print("File not found: ");
        serial_print(full_path);
        serial_print("\n");
    }
}
void cmd_write(const char *args)
{
    if (args[0] == '\0')
    {
        serial_print("Usage: write <filename> <content>\n");
        return;
    }

    // Split "filename content..." into filename and content
    char filename[64];
    size_t i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(filename) - 1)
    {
        filename[i] = args[i];
        i++;
    }
    filename[i] = '\0';

    const char *content = args + i;
    while (*content == ' ')
        content++; // skip the separating space

    char full_path[128];
    resolve_path(full_path, sizeof(full_path), filename);

    // Remove any existing version of the file first (overwrite semantics).
    // Ignored if it doesn't exist yet — that just means we're creating it.
    tar_remove(get_archive_mutable(), full_path);

    size_t content_len = 0;
    while (content[content_len])
        content_len++;

    if (!tar_append_file(get_archive_mutable(), fs_get_capacity(), full_path, content, content_len))
    {
        serial_print("write failed (out of space?)\n");
    }
}

void cmd_sync(void)
{
    /*if (fs_sync()) {
        serial_print("Filesystem synced to disk.\n");
    } else {
        serial_print("Sync failed.\n");
    }*/
}

static size_t edit_line_start(size_t pos)
{
    size_t i = pos;
    while (i > 0 && edit_buf[i - 1] != '\n')
        i--;
    return i;
}

static size_t edit_line_end(size_t pos, size_t len)
{
    size_t i = pos;
    while (i < len && edit_buf[i] != '\n')
        i++;
    return i;
}

void serial_print_uint(unsigned int value)
{
    char digits[10];
    int n = 0;
    if (value == 0)
    {
        serial_putc('0');
        return;
    }
    while (value > 0 && n < 10)
    {
        digits[n++] = '0' + (value % 10);
        value /= 10;
    }
    while (n > 0)
        serial_putc(digits[--n]);
}

// Positions the real terminal cursor to match `cursor`'s spot in the buffer.
static void edit_move_cursor_to(size_t cursor)
{
    size_t row = 0, col = 0;
    for (size_t i = 0; i < cursor; i++)
    {
        if (edit_buf[i] == '\n')
        {
            row++;
            col = 0;
        }
        else
            col++;
    }
    serial_print("\x1b[");
    serial_print_uint((unsigned int)(EDIT_HEADER_LINES + row));
    serial_print(";");
    serial_print_uint((unsigned int)(col + 1));
    serial_print("H");
}

static void edit_redraw(const char *path, size_t len, size_t cursor)
{
    serial_clear();
    serial_print("-- Editing ");
    serial_print(path);
    serial_print(" (Ctrl+S to save) --\n\n");
    for (size_t i = 0; i < len; i++)
    {
        serial_putc(edit_buf[i]);
    }
    edit_move_cursor_to(cursor);
}

void cmd_edit(const char *filename)
{
    if (filename[0] == '\0')
    {
        serial_print("Usage: edit <filename>\n");
        return;
    }

    char full_path[128];
    resolve_path(full_path, sizeof(full_path), filename);

    size_t len = 0;
    const void *archive = get_archive();
    if (archive != NULL)
    {
        size_t file_size;
        const void *data = tar_find(archive, full_path, &file_size);
        if (data != NULL)
        {
            if (file_size > EDIT_MAX_SIZE)
                file_size = EDIT_MAX_SIZE;
            for (size_t i = 0; i < file_size; i++)
            {
                edit_buf[i] = ((const char *)data)[i];
            }
            len = file_size;
        }
    }

    size_t cursor = len; // start at the end of the loaded file
    edit_redraw(full_path, len, cursor);

    for (;;)
    {
        char c = kb_getc();

        if (c == 0x1B)
        {
            char next = kb_getc();
            if (next != '[')
                continue; // not a sequence we handle
            char letter = kb_getc();

            if (letter == 'D')
            { // left
                if (cursor > 0)
                {
                    cursor--;
                    edit_move_cursor_to(cursor);
                }
            }
            else if (letter == 'C')
            { // right
                if (cursor < len)
                {
                    cursor++;
                    edit_move_cursor_to(cursor);
                }
            }
            else if (letter == 'A')
            { // up
                size_t line_start = edit_line_start(cursor);
                if (line_start > 0)
                {
                    size_t col = cursor - line_start;
                    size_t prev_line_end = line_start - 1;
                    size_t prev_line_start = edit_line_start(prev_line_end);
                    size_t prev_line_len = prev_line_end - prev_line_start;
                    cursor = prev_line_start + ((col < prev_line_len) ? col : prev_line_len);
                    edit_move_cursor_to(cursor);
                }
            }
            else if (letter == 'B')
            { // down
                size_t line_end = edit_line_end(cursor, len);
                if (line_end < len)
                {
                    size_t col = cursor - edit_line_start(cursor);
                    size_t next_line_start = line_end + 1;
                    size_t next_line_end = edit_line_end(next_line_start, len);
                    size_t next_line_len = next_line_end - next_line_start;
                    cursor = next_line_start + ((col < next_line_len) ? col : next_line_len);
                    edit_move_cursor_to(cursor);
                }
            }
            continue;
        }

        if (c == KEY_CTRL_S)
        {
            edit_move_cursor_to(len); // park below the text before prompting
            serial_print("\n\nSave changes? (y/n): ");
            char answer;
            for (;;)
            {
                answer = kb_getc();
                if (answer == 'y' || answer == 'Y' || answer == 'n' || answer == 'N')
                {
                    serial_putc(answer);
                    serial_print("\n");
                    break;
                }
            }

            if (answer == 'y' || answer == 'Y')
            {
                tar_remove(get_archive_mutable(), full_path);
                if (!tar_append_file(get_archive_mutable(), fs_get_capacity(), full_path, edit_buf, len))
                {
                    serial_print("Save failed (out of space?)\n");
                }
                else
                {
                    serial_print("Saved.\n");
                    cmd_sync();
                }
                return;
            }

            edit_redraw(full_path, len, cursor);
            continue;
        }

        if ((c == '\b' || c == 0x7F) && cursor > 0)
        {
            for (size_t i = cursor - 1; i < len - 1; i++)
            {
                edit_buf[i] = edit_buf[i + 1];
            }
            len--;
            cursor--;
            edit_redraw(full_path, len, cursor);
            continue;
        }

        if (c == '\r' || c == '\n')
        {
            if (len < EDIT_MAX_SIZE - 1)
            {
                for (size_t i = len; i > cursor; i--)
                    edit_buf[i] = edit_buf[i - 1];
                edit_buf[cursor] = '\n';
                len++;
                cursor++;
                edit_redraw(full_path, len, cursor);
            }
            continue;
        }

        if (len < EDIT_MAX_SIZE - 1 && c >= 0x20 && c < 0x7F)
        {
            for (size_t i = len; i > cursor; i--)
                edit_buf[i] = edit_buf[i - 1];
            edit_buf[cursor] = c;
            len++;
            cursor++;
            edit_redraw(full_path, len, cursor);
        }
    }
}

void kb_readline(char *buf, int max_len)
{
    int i = 0;

    for (;;)
    {
        char c = kb_getc();

        // Enter
        if (c == '\n' || c == '\r')
        {
            buf[i] = '\0';
            serial_print("\n");
            return;
        }

        // Backspace
        if ((c == '\b' || c == 0x7F) && i > 0)
        {
            i--;
            serial_print("\b \b");
            continue;
        }

        // Ctrl+D
        if (c == 4)
        {
            buf[i] = '\0';
            return;
        }

        // Normal character
        if (i < max_len - 1)
        {
            buf[i++] = c;
            serial_putc(c);
        }
    }
}