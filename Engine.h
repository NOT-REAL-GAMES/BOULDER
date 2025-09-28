#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

class Engine {
public:
    int argc;
    char** argv;

    SDL_Window* window;
    SDL_Event event{};


    Engine() {

        SDL_Init(SDL_INIT_VIDEO);

        auto window = SDL_CreateWindow("Boulder", 1280, 720, SDL_WINDOW_VULKAN);

    }

};