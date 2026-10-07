// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/ui/SdlApplication.hpp"

#include <memory>
#include <optional>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include "SdlCanvas.hpp"
#include "SdlImageService.hpp"
#include "SdlInput.hpp"
#include "akeno/core/Tasks.hpp"
#include "akeno/ui/AppController.hpp"
#include "akeno/ui/Screens.hpp"
#include "akeno/ui/Theme.hpp"
#include "akeno/ui/UiScript.hpp"

namespace akeno::ui {

using logging::logger;

namespace {

struct SdlSession {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    bool sdl = false;
    bool ttf = false;
    bool img = false;

    ~SdlSession() {
        if (renderer != nullptr) SDL_DestroyRenderer(renderer);
        if (window != nullptr) SDL_DestroyWindow(window);
        if (img) IMG_Quit();
        if (ttf) TTF_Quit();
        if (sdl) SDL_Quit();
    }
};

// Saves the current frame as logs/<name>.png (developer option --ui-script).
void saveScreenshot(SDL_Renderer* renderer, app::AppContext& context, const std::string& name) {
    const auto path = context.paths().logs() / (name + ".png");
    auto checked = context.fs().guard().checkWritable(path);
    if (!checked) {
        logger().error("ui", "screenshot refused: " + checked.error().describe());
        return;
    }
    int w = 0;
    int h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) return;
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) == 0 &&
        IMG_SavePNG(surface, checked.value().c_str()) == 0) {
        logger().info("ui", "screenshot saved: " + checked.value().string());
    } else {
        logger().error("ui", std::string("screenshot failed: ") + SDL_GetError());
    }
    SDL_FreeSurface(surface);
}

}  // namespace

int runSdlApplication(app::AppContext& context, const app::CommandLine& commandLine) {
    const bool console = context.platform().isConsole();
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    SdlSession session;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        logger().error("ui", std::string("SDL_Init failed: ") + SDL_GetError());
        return 1;
    }
    session.sdl = true;
    if (TTF_Init() != 0) {
        logger().error("ui", std::string("TTF_Init failed: ") + TTF_GetError());
        return 1;
    }
    session.ttf = true;
    session.img = (IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & IMG_INIT_PNG) != 0;
    if (!session.img) {
        logger().warn("ui", std::string("PNG support unavailable, icons will use placeholders: ") + IMG_GetError());
    }

    const int width = console ? theme::kScreenWidth : commandLine.windowWidth;
    const int height = console ? theme::kScreenHeight : commandLine.windowHeight;
    Uint32 windowFlags = SDL_WINDOW_SHOWN;
    if (!console) windowFlags |= SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (console || commandLine.fullscreen) windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    session.window = SDL_CreateWindow("Akeno PS5 Mod Manager", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width,
                                      height, windowFlags);
    if (session.window == nullptr) {
        logger().error("ui", std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        return 1;
    }
    // The console port renders through a software framebuffer.
    const Uint32 rendererFlags = console ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
    session.renderer = SDL_CreateRenderer(session.window, -1, rendererFlags);
    if (session.renderer == nullptr && !console) {
        session.renderer = SDL_CreateRenderer(session.window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (session.renderer == nullptr) {
        logger().error("ui", std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(session.renderer, theme::kScreenWidth, theme::kScreenHeight);
    SDL_RendererInfo info{};
    if (SDL_GetRendererInfo(session.renderer, &info) == 0) {
        logger().info("ui", std::string("renderer: ") + info.name);
    }

    std::optional<UiScriptRunner> script;
    if (!commandLine.uiScript.empty()) {
        auto steps = parseUiScript(commandLine.uiScript);
        if (!steps) {
            logger().error("ui", steps.error().describe());
            return 2;
        }
        script.emplace(std::move(steps).value());
    }

    TaskRunner tasks(2);
    MainThreadQueue mainQueue;
    int exitCode = 0;
    {
        sdl::SdlImageService images(session.renderer, tasks, mainQueue, 120);
        sdl::SdlCanvas canvas(session.renderer, &images);
        if (auto fonts = canvas.loadFonts(); !fonts) {
            logger().error("ui", fonts.error().describe());
            tasks.shutdown();
            return 1;
        }
        sdl::SdlInput input(console);
        input.openConnectedDevices();

        AppController controller(context, tasks, mainQueue, &images);
        ScreenHost host;
        setupScreens(host, controller.state());
        controller.start();
        logger().info("ui", "user interface started");

        const Uint64 frequency = SDL_GetPerformanceFrequency();
        const Uint64 startCounter = SDL_GetPerformanceCounter();
        auto now = [&] {
            return static_cast<double>(SDL_GetPerformanceCounter() - startCounter) / static_cast<double>(frequency);
        };

        bool running = true;
        bool dirty = true;
        std::vector<Action> actions;
        while (running) {
            const double t = now();
            UiEnv env{controller.state(), controller, t,
                      [&host, t](std::string text, ToastKind kind) { host.addToast(std::move(text), kind, t); }};

            actions.clear();
            SDL_Event event;
            // Idle politely when nothing changes: wait for input instead of spinning.
            const bool animating = host.animating(env) || mainQueue.pending() > 0;
            if (!dirty && !animating && SDL_WaitEventTimeout(&event, 50) == 1) {
                if (event.type == SDL_QUIT) running = false;
                input.handleEvent(event, t, actions);
                dirty = true;
            }
            while (SDL_PollEvent(&event) == 1) {
                if (event.type == SDL_QUIT) running = false;
                if (event.type == SDL_WINDOWEVENT) dirty = true;
                input.handleEvent(event, t, actions);
            }
            input.update(t, actions);
            std::vector<std::string> screenshots;
            if (script) {
                auto tick = script->update(t);
                actions.insert(actions.end(), tick.actions.begin(), tick.actions.end());
                screenshots = std::move(tick.screenshots);
                if (tick.quit) running = false;
                dirty = true;
            }
            for (Action action : actions) {
                host.handle(action, env);
                dirty = true;
            }
            if (mainQueue.drain() > 0) dirty = true;
            if (images.takeChanged()) dirty = true;
            if (controller.quitRequested()) running = false;

            if (dirty || host.animating(env)) {
                canvas.beginFrame();
                SDL_SetRenderDrawColor(session.renderer, 0, 0, 0, 255);
                SDL_RenderClear(session.renderer);
                host.render(canvas, env);
                for (const auto& name : screenshots) saveScreenshot(session.renderer, context, name);
                SDL_RenderPresent(session.renderer);
                dirty = false;
                if (!console) SDL_Delay(1);
            }
        }
        logger().info("ui", "user interface closing");
        // Workers may still reference the controller and the image service: stop them first.
        tasks.shutdown();
        mainQueue.drain(100000);
    }
    return exitCode;
}

}  // namespace akeno::ui
