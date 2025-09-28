#include "main.h"

Engine e;

int main(){    

    do {
        
        SDL_PollEvent(&e.event);    

	} while (e.event.type != SDL_EVENT_QUIT);


    return 0;
}