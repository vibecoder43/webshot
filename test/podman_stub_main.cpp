#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include <unistd.h>

int main(int argc, char *argv[])
{
    if (argc < 2)
        return 1;

    const std::string action = argv[1];

    if (action == "create") {
        bool fail = false;
        for (int i = 2; i < argc; i++) {
            if (std::string(argv[i]) == "fail") {
                fail = true;
                break;
            }
        }
        return fail ? 1 : 0;
    }

    if (action == "rm") {
        for (int i = 2; i < argc; i++) {
            const std::string arg = argv[i];
            if (arg == "fail-rm")
                return 1;
            if (arg == "fail-signal") {
                ::raise(SIGKILL);
                _exit(137);
            }
            if (arg == "fail-abort") {
                std::abort();
            }
        }
        return 0;
    }

    if (action == "start") {
        int exitCode = 0;
        for (int i = 2; i < argc; i++) {
            const std::string arg = argv[i];
            if (arg.rfind("emit-stdout=", 0) == 0) {
                const auto payload = arg.substr(std::string("emit-stdout=").size());
                std::cout << payload << '\n';
                continue;
            }
            if (arg.rfind("emit-stderr=", 0) == 0) {
                const auto payload = arg.substr(std::string("emit-stderr=").size());
                std::cerr << payload << '\n';
                continue;
            }
            if (arg.rfind("exit=", 0) == 0) {
                exitCode = std::atoi(arg.substr(std::string("exit=").size()).c_str());
                continue;
            }
        }
        std::cout.flush();
        std::cerr.flush();
        return exitCode;
    }

    return 0;
}
