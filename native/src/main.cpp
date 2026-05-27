#include "symbion_shell.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show_command) {
    SymbionShell shell(ResolveInitialUrl(command_line));
    return shell.Run(instance, show_command);
}
