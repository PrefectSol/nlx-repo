#include "core/config.h"
#include "core/credentials.h"
#include "supervisor.h"

#include <iostream>
#include <clocale>
#include <memory>

int main(int argc, const char *argv[])
{
    std::setlocale(LC_ALL, "C");

    if (argc > 2)
    {
        std::cerr << "nlx-server\n" << "example:\n" << argv[0] << " <path/to/config.cfg>(optional)" << std::endl;
        return 1;
    }

    try
    {
        const nlx::Credentials credentials = nlx::Credentials::load(".env");
        
        nlx::Config config;
        if (argc == 2)
        {
            config = nlx::Config::load(argv[1]);
        }

        std::unique_ptr<nlx::Supervisor> supervisor = std::make_unique<nlx::Supervisor>(config, credentials);
        return supervisor->start();
    }
    catch (const std::exception &exp)
    {
        std::cerr << "unexpected exception from supervisor:\n  " << exp.what() << std::endl;
    }

    return 1;
}

