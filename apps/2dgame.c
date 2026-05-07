#include "syscall.h"

#define MAP_W 28
#define MAP_H 14

static const char base_map[MAP_H][MAP_W + 1] = {
    "############################",
    "#@.....#...............$...#",
    "#.###..#.###########.####..#",
    "#...#..#.....#.....#....#..#",
    "###.#..#####.#.###.####.#..#",
    "#...#......#.#...#......#..#",
    "#.########.#.###.########..#",
    "#........#.#...#..........M#",
    "#.######.#.###.#############",
    "#......#.#.....#...........#",
    "######.#.#######.#########.#",
    "#$.....#.........#.......#X#",
    "#........#########.........#",
    "############################"
};

static char map[MAP_H][MAP_W + 1];

static int player_x = 1;
static int player_y = 1;
static int score = 0;
static int treasures_left = 0;
static int moves = 0;
static int game_over = 0;
static int won = 0;

static void copy_map(void) {
    treasures_left = 0;

    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            char c = base_map[y][x];
            map[y][x] = c;

            if (c == '@') {
                player_x = x;
                player_y = y;
                map[y][x] = '.';
            }

            if (c == '$') {
                treasures_left++;
            }
        }

        map[y][MAP_W] = 0;
    }
}

static void print_number(int n) {
    if (n < 0) {
        app_putchar('-');
        n = -n;
    }

    app_print_dec32((u32)n);
}

static void wait_for_any_key(void) {
    app_read_key();
}

static void short_pause(void) {
    app_sleep(700);
}

static void draw_game(void) {
    app_clear();

    app_write("================================\n");
    app_write("        NAQ DUNGEON QUEST       \n");
    app_write("================================\n\n");

    for (int y = 0; y < MAP_H; y++) {
        app_write("  ");

        for (int x = 0; x < MAP_W; x++) {
            if (x == player_x && y == player_y) {
                app_putchar('@');
            } else {
                app_putchar(map[y][x]);
            }
        }

        app_write("\n");
    }

    app_write("\nScore: ");
    print_number(score);

    app_write("   Treasures left: ");
    print_number(treasures_left);

    app_write("   Moves: ");
    print_number(moves);

    app_write("\n\nControls: W/A/S/D move, Q quit\n");
    app_write("Goal: collect $ then reach X. Avoid M.\n");
}

static int is_walkable(char c) {
    if (c == '#') {
        return 0;
    }

    return 1;
}

static void move_player(int dx, int dy) {
    int nx = player_x + dx;
    int ny = player_y + dy;

    if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) {
        return;
    }

    char target = map[ny][nx];

    if (!is_walkable(target)) {
        return;
    }

    moves++;

    if (target == '$') {
        score += 100;
        treasures_left--;
        map[ny][nx] = '.';
    }

    if (target == 'M') {
        game_over = 1;
        won = 0;
        return;
    }

    if (target == 'X') {
        if (treasures_left == 0) {
            game_over = 1;
            won = 1;
            return;
        }

        app_write("\nYou need all treasures before exiting!\n");
        short_pause();
        return;
    }

    player_x = nx;
    player_y = ny;
}

static void show_intro(void) {
    app_clear();

    app_write("================================\n");
    app_write("        NAQ DUNGEON QUEST       \n");
    app_write("================================\n\n");
    app_write("You wake up inside an old text-mode dungeon.\n");
    app_write("Collect every treasure and find the exit.\n\n");
    app_write("Legend:\n");
    app_write("  @  You\n");
    app_write("  #  Wall\n");
    app_write("  $  Treasure\n");
    app_write("  X  Exit\n");
    app_write("  M  Monster\n\n");
    app_write("Press any key to start...");

    wait_for_any_key();
}

static void show_ending(void) {
    app_clear();

    if (won) {
        app_write("================================\n");
        app_write("              YOU WIN!          \n");
        app_write("================================\n\n");
        app_write("You escaped the dungeon with all treasure.\n");
    } else {
        app_write("================================\n");
        app_write("             GAME OVER          \n");
        app_write("================================\n\n");
        app_write("The monster got you.\n");
    }

    app_write("\nFinal score: ");
    print_number(score);

    app_write("\nMoves: ");
    print_number(moves);

    app_write("\n\nPress any key to return to KER...");
    wait_for_any_key();
}

void app_main(void) {
    copy_map();

    score = 0;
    moves = 0;
    game_over = 0;
    won = 0;

    app_set_raw_input(1);

    show_intro();

    while (!game_over) {
        draw_game();

        int key = app_read_key();

        if (key == 'q' || key == 'Q') {
            break;
        }
        else if (key == 'w' || key == 'W') {
            move_player(0, -1);
        }
        else if (key == 's' || key == 'S') {
            move_player(0, 1);
        }
        else if (key == 'a' || key == 'A') {
            move_player(-1, 0);
        }
        else if (key == 'd' || key == 'D') {
            move_player(1, 0);
        }
    }

    if (game_over) {
        show_ending();
    }

    app_set_raw_input(0);
}