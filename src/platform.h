#ifndef PLATFORM_H
#define PLATFORM_H

// Prepares the process for the current platform. On Windows, it replaces the arguments
// with their UTF-8 version and lets the console display UTF-8 text and ANSI escape sequences.
void platform_init(int& argc, char**& argv);

#endif // PLATFORM_H
