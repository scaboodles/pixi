#include <ncurses.h>
#include <stdio.h>
int main(){
    initscr();
    cbreak();
    noecho();
    curs_set(0);

    int term_height, term_width;
    getmaxyx(stdscr, term_height, term_width);
    endwin();
    printf("h: %d, w: %d", term_height * 2, term_width);
}
