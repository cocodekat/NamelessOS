// games.c

#include "games.h"
#include "flappy_bird.h"
#include "platformer.h"
#include "serial.h"
#include "fb.h"
#include "helpers.h"

typedef struct
{
    const char *name;
    int (*run)(void);
} game_t;

static const game_t builtin_games[] = {
    {"flappy", flappy_bird_run},
    {"platformer", platformer_run},
};

#define GAME_COUNT (sizeof(builtin_games) / sizeof(builtin_games[0]))

static const game_t *find_game(const char *name)
{
    for (unsigned i = 0; i < GAME_COUNT; i++)
    {
        if (streq(name, builtin_games[i].name))
            return &builtin_games[i];
    }

    return 0;
}

static void games_list(void)
{
    serial_print("Installed games:\n");

    for (unsigned i = 0; i < GAME_COUNT; i++)
    {
        serial_print("  ");
        serial_print(builtin_games[i].name);
        serial_print("\n");
    }
}

void games_command(char *args)
{
    if (!args || args[0] == '\0')
    {
        serial_print("Usage:\n");
        serial_print("  games list\n");
        serial_print("  games run <game>\n");
        return;
    }

    // games list
    if (streq(args, "list"))
    {
        games_list();
        return;
    }

    // games run ...
    if (args[0] == 'r' &&
        args[1] == 'u' &&
        args[2] == 'n' &&
        args[3] == ' ')
    {

        char *name = args + 4;

        if (*name == '\0')
        {
            serial_print("Usage: games run <game>\n");
            return;
        }

        const game_t *game = find_game(name);

        if (!game)
        {
            serial_print("Game not found: ");
            serial_print(name);
            serial_print("\n");
            return;
        }

        game->run();

        // Game overwrote the framebuffer, so restore console.
        fb_console_init();
        serial_clear();

        return;
    }

    serial_print("Unknown games command.\n");
    serial_print("Usage:\n");
    serial_print("  games list\n");
    serial_print("  games run <game>\n");
}
