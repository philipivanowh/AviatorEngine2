#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>

#include "core/App.h"

SDL_AppResult SDL_AppInit(void** /*appstate*/, int /*argc*/, char* /*argv*/[])
{
    return App::Initialize();
}

SDL_AppResult SDL_AppIterate(void* /*appstate*/)
{
    return App::RunFrame();
}

SDL_AppResult SDL_AppEvent(void* /*appstate*/, SDL_Event* event)
{
    return App::ProcessInput(event);
}

void SDL_AppQuit(void* /*appstate*/, SDL_AppResult /*result*/)
{
    App::Shutdown();
}