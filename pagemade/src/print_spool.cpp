/*
 * pagemade/print_spool.cpp — CUPS through the lp and lpstat commands.
 *
 * Linux and macOS both ship those. The dialog never runs them. A printer
 * name is one argument, never a shell string.
 */
#include "print.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace pagemade {
namespace {

struct Spawned {
    int code = -1;
    std::string out, err;
};

bool command_on_path(const char *name) {
    const char *env = std::getenv("PATH");
    if (!env || !*env || !name || !*name)
        return false;
    const std::string path(env);
    size_t i = 0;
    while (i <= path.size()) {
        const size_t colon = path.find(':', i);
        const std::string dir = path.substr(i, colon == std::string::npos ? std::string::npos
                                                                           : colon - i);
        if (!dir.empty()) {
            const std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0)
                return true;
        }
        if (colon == std::string::npos)
            break;
        i = colon + 1;
    }
    return false;
}

void read_ready(int fd, std::string &into, bool &open) {
    char buf[4096];
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n > 0)
        into.append(buf, (size_t) n);
    else if (!(n < 0 && errno == EINTR))
        open = false;
}

Spawned run(const std::vector<std::string> &args) {
    Spawned result;
    if (args.empty())
        return result;
    int outfd[2] = {-1, -1}, errfd[2] = {-1, -1};
    if (pipe(outfd) != 0 || pipe(errfd) != 0) {
        if (outfd[0] >= 0) { close(outfd[0]); close(outfd[1]); }
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(outfd[0]); close(outfd[1]);
        close(errfd[0]); close(errfd[1]);
        return result;
    }
    if (pid == 0) {
        dup2(outfd[1], STDOUT_FILENO);
        dup2(errfd[1], STDERR_FILENO);
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0)
            dup2(devnull, STDIN_FILENO);
        close(outfd[0]); close(outfd[1]);
        close(errfd[0]); close(errfd[1]);
        if (devnull > 2)
            close(devnull);
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const std::string &s : args)
            argv.push_back(const_cast<char *>(s.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(outfd[1]);
    close(errfd[1]);
    bool out_open = true, err_open = true;
    while (out_open || err_open) {
        pollfd fds[2];
        nfds_t n = 0;
        int out_i = -1, err_i = -1;
        if (out_open) {
            fds[n] = {outfd[0], POLLIN, 0};
            out_i = (int) n++;
        }
        if (err_open) {
            fds[n] = {errfd[0], POLLIN, 0};
            err_i = (int) n++;
        }
        if (poll(fds, n, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (out_i >= 0 && (fds[out_i].revents & (POLLIN | POLLHUP | POLLERR)))
            read_ready(outfd[0], result.out, out_open);
        if (err_i >= 0 && (fds[err_i].revents & (POLLIN | POLLHUP | POLLERR)))
            read_ready(errfd[0], result.err, err_open);
    }
    close(outfd[0]);
    close(errfd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    result.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

std::string trim_line(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        const size_t nl = text.find('\n', i);
        std::string line = trim_line(text.substr(i, nl == std::string::npos ? std::string::npos
                                                                             : nl - i));
        if (!line.empty())
            out.push_back(std::move(line));
        if (nl == std::string::npos)
            break;
        i = nl + 1;
    }
    return out;
}

/* "printer Name is idle." / "printer Name disabled since ..." */
std::vector<std::string> names_from_lpstat_p(const std::string &text) {
    std::vector<std::string> names;
    for (const std::string &line : lines_of(text)) {
        if (line.compare(0, 8, "printer ") != 0)
            continue;
        const size_t name = 8;
        const size_t end = line.find(' ', name);
        if (end == name || end == std::string::npos)
            continue;
        names.push_back(line.substr(name, end - name));
    }
    return names;
}

std::string default_from_lpstat_d(const std::string &text) {
    const std::string key = "system default destination:";
    for (const std::string &line : lines_of(text)) {
        if (line.compare(0, key.size(), key) != 0)
            continue;
        return trim_line(line.substr(key.size()));
    }
    return {};
}

bool name_ok(const std::string &name) {
    if (name.empty() || name[0] == '-')
        return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f)
            return false;
    return true;
}

} // namespace

bool printing_available() { return command_on_path("lp"); }

const char *printing_unavailable_reason() {
    return "The lp command isn't on PATH. Export PDF and print it from another program.";
}

std::vector<Printer> list_printers() {
    std::vector<std::string> names;
    if (command_on_path("lpstat")) {
        const Spawned listed = run({"lpstat", "-e"});
        if (listed.code == 0)
            names = lines_of(listed.out);
        if (names.empty()) {
            const Spawned described = run({"lpstat", "-p"});
            if (described.code == 0)
                names = names_from_lpstat_p(described.out);
        }
    }
    const std::string fallback = command_on_path("lpstat")
                                     ? default_from_lpstat_d(run({"lpstat", "-d"}).out)
                                     : std::string();
    std::vector<Printer> out;
    for (const std::string &name : names) {
        if (!name_ok(name))
            continue;
        bool seen = false;
        for (const Printer &have : out)
            if (have.name == name)
                seen = true;
        if (seen)
            continue;
        Printer p;
        p.name = name;
        p.label = name;
        p.is_default = !fallback.empty() && name == fallback;
        out.push_back(std::move(p));
    }
    return out;
}

std::string print_pdf(const std::string &path, const PrintRequest &request) {
    if (!printing_available())
        return printing_unavailable_reason();
    if (path.empty())
        return "there is no PDF to print";
    if (!request.printer.empty() && !name_ok(request.printer))
        return "that printer name can't be used";
    int copies = request.copies;
    if (copies < 1)
        copies = 1;
    if (copies > 99)
        copies = 99;
    std::vector<std::string> args = {"lp", "-n", std::to_string(copies)};
    if (!request.printer.empty()) {
        args.push_back("-d");
        args.push_back(request.printer);
    }
    if (!request.title.empty()) {
        args.push_back("-t");
        args.push_back(request.title);
    }
    args.push_back(path);
    const Spawned job = run(args);
    if (job.code == 0)
        return {};
    std::string why = trim_line(job.err.empty() ? job.out : job.err);
    if (why.empty())
        why = job.code == 127 ? "couldn't run lp" : "lp did not accept the job";
    return why;
}

} // namespace pagemade
