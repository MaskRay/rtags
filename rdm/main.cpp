#include <QCoreApplication>
#include <QThread>
#include <QThreadPool>
#include "Server.h"
#include <getopt.h>
#include <QDateTime>
#include <stdio.h>
#include <stdlib.h>
#include <Log.h>
#include <RTags.h>

void usage(FILE *f)
{
    fprintf(f,
            "rdm ...options...\n"
            "  --help|-h               Display this page\n"
            "  --include-path|-I [arg] Add additional include path to clang\n"
            "  --include|-i [arg]      Add additional include directive to clang\n"
            "  --define|-D [arg]       Add additional define directive to clang\n"
            "  --log-file|-L [arg]     Log to this file\n"
            "  --append|-A             Append to log file\n"
            "  --verbose|-v            Change verbosity, multiple -v's are allowed\n"
            "  --clean-slate|-C        Start from a clean slate\n"
            "  --thread-count|-j [arg] Spawn this many threads for thread pool\n");
}

int main(int argc, char** argv)
{
    struct option opts[] = {
        { "help", no_argument, 0, 'h' },
        { "include-path", required_argument, 0, 'I' },
        { "include", required_argument, 0, 'i' },
        { "define", required_argument, 0, 'D' },
        { "log-file", required_argument, 0, 'L' },
        { "append", no_argument, 0, 'A' },
        { "verbose", no_argument, 0, 'v' },
        { "thread-count", required_argument, 0, 'j' },
        { "clean-slate", no_argument, 0, 'C' },
        { 0, 0, 0, 0 }
    };

    int jobs = QThread::idealThreadCount();
    unsigned options = 0;
    QList<QByteArray> defaultArguments;
    const char *logFile = 0;
    unsigned logFlags = 0;
    int logLevel = 0;
    const QByteArray shortOptions = RTags::shortOptions(opts);

    forever {
        const int c = getopt_long(argc, argv, shortOptions.constData(), opts, 0);
        if (c == -1)
            break;
        switch (c) {
        case 'h':
            usage(stdout);
            return 0;
        case 'C':
            RTags::removeDirectory(ASTPATH);
            break;
        case 'j':
            jobs = atoi(optarg);
            if (jobs <= 0) {
                fprintf(stderr, "Can't parse argument to -j %s", optarg);
                return 1;
            }
            break;
        case 'D':
            defaultArguments.append("-D" + QByteArray(optarg));
            break;
        case 'I':
            defaultArguments.append("-I" + QByteArray(optarg));
            break;
        case 'i':
            defaultArguments.append("-include");
            defaultArguments.append(optarg);
            break;
        case 'A':
            logFlags |= Append;
            break;
        case 'L':
            logFile = optarg;
            break;
        case 'v':
            ++logLevel;
            break;
        case '?':
            usage(stderr);
            return 1;
        }
    }
    QThreadPool::globalInstance()->setMaxThreadCount(jobs);
    QCoreApplication app(argc, argv);
    if (!initLogging(logLevel, logFile, logFlags)) {
        fprintf(stderr, "Can't initialize logging with %d %s 0x%0x\n",
                logLevel, logFile ? logFile : "", logFlags);
        return false;
    }

    warning("Running with %d jobs", jobs);

    Server server;
    if (!server.init(options, defaultArguments))
        return 1;

    return app.exec();
}
