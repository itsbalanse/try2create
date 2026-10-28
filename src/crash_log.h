#pragma once
// A crash and hang logger. When the program crashes (an access violation, an uncaught C++ exception, abort()...) it writes a report to
// %LOCALAPPDATA%\Try2Create\crash\: the exception, where it happened (function and line when Try2Create.pdb sits next to the program), the stack, the
// last things you did (the "breadcrumbs") and a .dmp file. When the window stops answering for 15 seconds it writes the same for the frozen thread
// (hang_*.txt / .dmp) while the program is still running, so a freeze that you end from Task Manager still leaves a report.
#include <string>

void InstallCrashLogger();                // first thing in main
void CrashHeartbeat();                    // once per turn of the main loop (tells the watchdog the program is alive)
void CrashNote(const std::string& what);  // a breadcrumb: "tab: Vision board", "dropped a picture on the board"...
std::wstring CrashLogDir();
