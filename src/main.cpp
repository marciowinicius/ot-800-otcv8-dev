/*
 * Copyright (c) 2010-2017 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <framework/core/application.h>
#include <framework/core/resourcemanager.h>
#include <framework/core/eventdispatcher.h>
#include <framework/luaengine/luainterface.h>
#include <framework/http/http.h>
#include <framework/platform/crashhandler.h>
#include <framework/platform/platformwindow.h>
#include <client/client.h>
#include <client/game.h>

#include <windows.h>
#include <tlhelp32.h>
#include <algorithm> 
#include <cctype>
#include <sstream> 
#include <unordered_set> 
#include <iostream>
#include <vector>
#include <string>
#include <thread>

// BY LUCKEZ

// Manipulador de exceções
LONG WINAPI antiDissembler(EXCEPTION_POINTERS* ExceptionInfo) {
    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        std::cerr << "Tentativa de disassemble detectada. Encerrando o programa.\n";
        ExitProcess(1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool saoSimilares(const std::string& str1, const std::string& str2) {
    // Convertendo ambas as strings para minúsculas para comparação sem diferenciação de maiúsculas
    std::string str1Min = str1;
    std::string str2Min = str2;
    std::transform(str1Min.begin(), str1Min.end(), str1Min.begin(), [](unsigned char c) { return std::tolower(c); });
    std::transform(str2Min.begin(), str2Min.end(), str2Min.begin(), [](unsigned char c) { return std::tolower(c); });

    // Removendo espaços em branco e caracteres especiais
    str1Min.erase(std::remove_if(str1Min.begin(), str1Min.end(), [](unsigned char c) { return std::isspace(c) || !std::isalnum(c); }), str1Min.end());
    str2Min.erase(std::remove_if(str2Min.begin(), str2Min.end(), [](unsigned char c) { return std::isspace(c) || !std::isalnum(c); }), str2Min.end());

    // Verificando se uma string está contida na outra após remoção de espaços e caracteres especiais
    return str1Min.find(str2Min) != std::string::npos || str2Min.find(str1Min) != std::string::npos;
}

bool isBotExecutableRunning(const std::vector<std::string>& botProcessNames) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPMODULE, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        std::cerr << "Erro: CreateToolhelp32Snapshot falhou." << std::endl;
        return false;
    }

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);

    if (!Process32First(hSnapshot, &pe32)) {
        CloseHandle(hSnapshot);
        std::cerr << "Erro: Process32First falhou." << std::endl;
        return false;
    }

    do {
        std::string processName = pe32.szExeFile;
        std::transform(processName.begin(), processName.end(), processName.begin(), ::tolower);
        
        // Verifica se o nome do processo corresponde a algum nome de bot
        for (const auto& botName : botProcessNames) {
            if (processName.find(botName) != std::string::npos) {
                CloseHandle(hSnapshot);
                return true;
            }
        }

        // Verifica se o nome do módulo do processo corresponde a algum nome de bot
        MODULEENTRY32 me32;
        me32.dwSize = sizeof(MODULEENTRY32);
        if (Module32First(hSnapshot, &me32)) {
            do {
                std::string moduleName = me32.szModule;
                std::transform(moduleName.begin(), moduleName.end(), moduleName.begin(), ::tolower);
                
                for (const auto& botName : botProcessNames) {
                    if (moduleName.find(botName) != std::string::npos) {
                        CloseHandle(hSnapshot);
                        return true;
                    }
                }
            } while (Module32Next(hSnapshot, &me32));
        }
    } while (Process32Next(hSnapshot, &pe32));

    CloseHandle(hSnapshot);
    return false;
}

// Função para encerrar o cliente
void terminateClient() {
    g_logger.fatal("Dont RUN BOT BRO");
    std::cerr << "Client terminated." << std::endl;
}

void checkForBots() {
    std::vector<std::string> botProcessNames = { "bot", "rift", "autohotkey", "uopilot", "pinador", "autohotkey", "xmousebutton", "macro" };
    if (isBotExecutableRunning(botProcessNames)) {
        terminateClient();
        return;
    }
    g_dispatcher.scheduleEventEx("PeriodicCheckForBots", [&] {
        checkForBots();
    }, 30000);
    return;
}

int main(int argc, const char* argv[]) {
    std::vector<std::string> args(argv, argv + argc);

#ifdef CRASH_HANDLER
    installCrashHandler();
#endif

    // initialize resources
    g_resources.init(argv[0]);
    std::string compactName = g_resources.getCompactName();
    g_logger.setLogFile(compactName + ".log");

    // setup application name and version
    g_app.setName("OTClientV8");
    g_app.setCompactName(compactName);
    g_app.setVersion("3.2");

    g_game.checkProcess();
    checkForBots();

#ifdef WITH_ENCRYPTION
    if (std::find(args.begin(), args.end(), "--encrypt") != args.end()) {
        g_lua.init();
        g_resources.encrypt(args.size() >= 3 ? args[2] : "");
        std::cout << "Encryption complete" << std::endl;
#ifdef WIN32
        MessageBoxA(NULL, "Encryption complete", "Success", 0);
#endif
        return 0;
    }
#endif

    if (g_resources.launchCorrect(g_app.getName(), g_app.getCompactName())) {
        return 0; // started other executable
    }

    // initialize application framework and otclient
    g_app.init(args);
    g_client.init(args);
    g_http.init();

    bool testMode = std::find(args.begin(), args.end(), "--test") != args.end();
    if (testMode) {
        g_logger.setTestingMode();    
    }

    // find script init.lua and run it
    g_resources.setupWriteDir(g_app.getName(), g_app.getCompactName());
    g_resources.setup();

    SetUnhandledExceptionFilter(antiDissembler);
    
    if (!g_lua.safeRunScript("init.lua")) {
        if (g_resources.isLoadedFromArchive() && !g_resources.isLoadedFromMemory() &&
            g_resources.loadDataFromSelf(true)) {
            g_logger.error("Unable to run script init.lua! Trying to run version from memory.");
            if (!g_lua.safeRunScript("init.lua")) {
                g_resources.deleteFile("data.zip"); // remove incorrect data.zip
                g_logger.fatal("Unable to run script init.lua from binary file!\nTry to run client again.");
            }
        } else {
            g_logger.fatal("Unable to run script init.lua!");
        }
    }

    if (testMode) {
        if (!g_lua.safeRunScript("test.lua")) {
            g_logger.fatal("Can't run test.lua");
        }
    }

#ifdef WIN32
    // support for progdn proxy system, if you don't have this dll nothing will happen
    // however, it is highly recommended to use otcv8 proxy system
    LoadLibraryA("progdn32.dll");
#endif

    // the run application main loop
    g_app.run();

#ifdef CRASH_HANDLER
    uninstallCrashHandler();
#endif

    // unload modules
    g_app.deinit();

    // terminate everything and free memory
    g_http.terminate();
    g_client.terminate();
    g_app.terminate();
    return 0;
}

#ifdef ANDROID
#include <framework/platform/androidwindow.h>

android_app* g_androidState = nullptr;
void android_main(struct android_app* state)
{
    g_mainThreadId = g_dispatcherThreadId = g_graphicsThreadId = std::this_thread::get_id();
    g_androidState = state;

    state->userData = nullptr;
    state->onAppCmd = +[](android_app* app, int32_t cmd) -> void {
       return g_androidWindow.handleCmd(cmd);
    };
    state->onInputEvent = +[](android_app* app, AInputEvent* event) -> int32_t {
        return g_androidWindow.handleInput(event);
    };
    state->activity->callbacks->onNativeWindowResized = +[](ANativeActivity* activity, ANativeWindow* window) -> void {
        g_graphicsDispatcher.scheduleEventEx("updateWindowSize", [] {
            g_androidWindow.updateSize();
        }, 500);
    };
    state->activity->callbacks->onContentRectChanged = +[](ANativeActivity* activity, const ARect* rect) -> void {
        g_graphicsDispatcher.scheduleEventEx("updateWindowSize", [] {
            g_androidWindow.updateSize();
        }, 500);
    };

    bool terminated = false;
    g_window.setOnClose([&] {
        terminated = true;
    });
    while(!g_window.isVisible() && !terminated)
        g_window.poll(); // init window
    // run app
    const char* args[] = { "otclientv8.apk" };
    main(1, args);
    std::exit(0); // required!
}
#endif
