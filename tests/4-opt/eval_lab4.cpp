// ReSharper disable CppClangTidyPerformanceInefficientStringConcatenation
#include <climits>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <list>
#include <optional>
#include <regex>
#include <string>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>

using namespace std;

enum evaluate_result : uint8_t { SUCCESS, FAIL, PASS };

static enum stage : uint8_t
{
    raw, mem2reg, licm, all
} STAGE;

static string TEST_PATH;

static enum test_type : uint8_t
{
    debug, test
} TYPE;

struct cmd_result
{
    string out_str;
    string err_str;
    int ret_val;

    bool have_err_message() const
    {
        return !err_str.empty();
    }
};

struct cmd_result_mix
{
    string str;
    int ret_val;
};

static string red(const string& str) { return "\033[31;1m" + str + "\033[0m"; }

static string green(const string& str) { return "\033[32;1m" + str + "\033[0m"; }

static ofstream ost;

static void ext(int flag)
{
    ost.close();
    exit(flag);
}

static void out(const string& str, bool to_std)
{
    if (to_std) cout << str;
    else ost << str;
}

static void out2(const string& str)
{
    cout << str;
    ost << str;
}

static void out2e(const string& str)
{
    cout << red(str);
    ost << str;
}

static std::pair<std::string, std::string> readPipeLine(int outpipe[], int errpipe[], int limit)
{
    fd_set readfds;
    int maxFd = std::max(outpipe[0], errpipe[0]);
    char buffer[1024];
    std::string stdout_result;
    std::string stderr_result;
    int totalRead = 0;
    bool have_stdout = true;
    bool have_stderr = true;

    while (true)
    {
        FD_ZERO(&readfds);

        if (have_stdout)
            FD_SET(outpipe[0], &readfds);
        if (have_stderr)
            FD_SET(errpipe[0], &readfds);

        int ret = select(maxFd + 1, &readfds, nullptr, nullptr, nullptr);
        if (ret == -1)
        {
            out("select() 调用失败!\n", true);
            out("select() 调用失败!\n", false);
            ext(-1);
        }
        if (ret == 0)
            continue;

        if (FD_ISSET(outpipe[0], &readfds))
        {
            int bytes = static_cast<int>(read(outpipe[0], buffer, sizeof(buffer) - 1));
            if (bytes > 0)
            {
                int writeLen = std::min(bytes, limit - totalRead);
                buffer[writeLen] = '\0';
                stdout_result += buffer;
                totalRead += writeLen;

                if (totalRead >= limit)
                    break;
            }
            else
            {
                FD_CLR(outpipe[0], &readfds);
                close(outpipe[0]);
                have_stdout = false;
                maxFd = have_stderr ? errpipe[0] : -1;
            }
        }

        if (FD_ISSET(errpipe[0], &readfds))
        {
            int bytes = static_cast<int>(read(errpipe[0], buffer, sizeof(buffer) - 1));
            if (bytes > 0)
            {
                int writeLen = std::min(bytes, limit - totalRead);
                buffer[writeLen] = '\0';
                stderr_result += buffer;
                totalRead += writeLen;

                if (totalRead >= limit)
                    break;
            }
            else
            {
                FD_CLR(errpipe[0], &readfds);
                close(errpipe[0]);
                have_stderr = false;
                maxFd = have_stdout ? outpipe[0] : -1;
            }
        }

        if (!FD_ISSET(outpipe[0], &readfds) && !FD_ISSET(errpipe[0], &readfds))
            break;
    }

    if (!stdout_result.empty() && stdout_result.back() != '\n') stdout_result += '\n';
    if (!stderr_result.empty() && stderr_result.back() != '\n') stderr_result += '\n';

    return {stdout_result, stderr_result};
}

static std::string readPipeLineMix(int outpipe[], int errpipe[], int limit)
{
    fd_set readfds;
    int maxFd = std::max(outpipe[0], errpipe[0]);
    char buffer[1024];
    std::string result;
    int totalRead = 0;
    bool have_stdout = true;
    bool have_stderr = true;

    while (true)
    {
        FD_ZERO(&readfds);

        if (have_stdout)
            FD_SET(outpipe[0], &readfds);
        if (have_stderr)
            FD_SET(errpipe[0], &readfds);

        int ret = select(maxFd + 1, &readfds, nullptr, nullptr, nullptr);
        if (ret == -1)
        {
            out("select() 调用失败!\n", true);
            out("select() 调用失败!\n", false);
            ext(-1);
        }
        if (ret == 0)
            continue;

        if (FD_ISSET(outpipe[0], &readfds))
        {
            int bytes = static_cast<int>(read(outpipe[0], buffer, sizeof(buffer) - 1));
            if (bytes > 0)
            {
                int writeLen = std::min(bytes, limit - totalRead);
                buffer[writeLen] = '\0';
                result += buffer;
                totalRead += writeLen;

                if (totalRead >= limit)
                    break;
            }
            else
            {
                FD_CLR(outpipe[0], &readfds);
                close(outpipe[0]);
                have_stdout = false;
                maxFd = have_stderr ? errpipe[0] : -1;
            }
        }

        if (FD_ISSET(errpipe[0], &readfds))
        {
            int bytes = static_cast<int>(read(errpipe[0], buffer, sizeof(buffer) - 1));
            if (bytes > 0)
            {
                int writeLen = std::min(bytes, limit - totalRead);
                buffer[writeLen] = '\0';
                result += buffer;
                totalRead += writeLen;

                if (totalRead >= limit)
                    break;
            }
            else
            {
                FD_CLR(errpipe[0], &readfds);
                close(errpipe[0]);
                have_stderr = false;
                maxFd = have_stdout ? outpipe[0] : -1;
            }
        }

        if (!FD_ISSET(outpipe[0], &readfds) && !FD_ISSET(errpipe[0], &readfds))
            break;
    }

    if (!result.empty() && result.back() != '\n') result += '\n';

    return result;
}

static cmd_result runCommand(const std::string& command, int limit = INT_MAX)
{
    int outpipe[2];
    int errpipe[2];
    if (pipe(outpipe) != 0 || pipe(errpipe) != 0)
    {
        out2("pipe() 调用失败!\n");
        ext(-1);
    }

    pid_t pid = fork();
    if (pid == -1)
    {
        out2("fork() 调用失败!\n");
        ext(-1);
    }

    if (pid == 0)
    {
        close(outpipe[0]);
        close(errpipe[0]);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(errpipe[1], STDERR_FILENO);
        execlp("sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        perror("execlp");
        ext(127);
    }

    close(outpipe[1]);
    close(errpipe[1]);

    auto res = readPipeLine(outpipe, errpipe, limit);

    int status;
    waitpid(pid, &status, 0);

    cmd_result ret;

    ret.out_str = res.first;
    ret.err_str = res.second;
    ret.ret_val = WEXITSTATUS(status);

    return ret;
}

static cmd_result_mix runCommandMix(const std::string& command, int limit = INT_MAX)
{
    int outpipe[2];
    int errpipe[2];
    if (pipe(outpipe) != 0 || pipe(errpipe) != 0)
    {
        out2("pipe() 调用失败!\n");
        ext(-1);
    }

    pid_t pid = fork();
    if (pid == -1)
    {
        out2("fork() 调用失败!\n");
        ext(-1);
    }

    if (pid == 0)
    {
        close(outpipe[0]);
        close(errpipe[0]);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(errpipe[1], STDERR_FILENO);
        execlp("sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        perror("execlp");
        ext(127);
    }

    close(outpipe[1]);
    close(errpipe[1]);

    auto res = readPipeLineMix(outpipe, errpipe, limit);

    int status;
    waitpid(pid, &status, 0);

    cmd_result_mix ret;

    ret.str = res;
    ret.ret_val = WEXITSTATUS(status);

    return ret;
}

static list<string> splitString(const string& str)
{
    list<string> lines;
    std::istringstream stream(str);
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

static std::string readFile(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file.is_open())
    {
        cerr << "无法打开文件 " + filename << '\n';
        ext(-1);
    }

    std::string content((std::istreambuf_iterator(file)),
                        std::istreambuf_iterator<char>());
    file.close();
    return content;
}

static void writeFile(const std::string& content, const std::string& filename)
{
    std::ofstream file(filename);
    if (!file.is_open())
    {
        cerr << "无法打开文件 " + filename << '\n';
        ext(-1);
    }

    file << content;
    file.close();
}

static string lastDotLeft(const string& str)
{
    size_t lastDotPos = str.find_last_of('.');
    if (lastDotPos == string::npos)
    {
        return str;
    }
    return str.substr(0, lastDotPos);
}

static string lastLineRight(const string& str)
{
    size_t lastSlashPos = str.find_last_of('/');
    if (lastSlashPos == string::npos)
    {
        return str;
    }
    return str.substr(lastSlashPos + 1);
}

static void makeDir(const string& dirPath)
{
    if (filesystem::exists(dirPath))
        return;
    if (!filesystem::create_directories(dirPath))
    {
        out2("目录 " + dirPath + " 创建失败！\n");
        ext(-1);
    }
}

static const char* ERR_LOG = R"(Usage: ./eval_lab4.sh [test-stage] [path-to-testcases] [type]
test-stage: 'raw' or 'licm' or 'mem2reg' or 'all'
path-to-testcases: './testcases/functional-cases' or '../testcases_general' or 'self made cases'
type: 'debug' or 'test', debug will output .ll file
)";

static int parseCmd(int argc, char* argv[])
{
    if (argc != 4)
    {
        out(ERR_LOG, true);
        return -1;
    }
    if (std::strcmp(argv[1], "licm") == 0)
    {
        STAGE = licm;
        ost.open("licm_log.txt", ios::out);
    }
    else if (std::strcmp(argv[1], "mem2reg") == 0)
    {
        STAGE = mem2reg;
        ost.open("mem2reg_log.txt", ios::out);
    }
    else if (std::strcmp(argv[1], "raw") == 0)
    {
        STAGE = raw;
        ost.open("raw_log.txt", ios::out);
    }
    else if (std::strcmp(argv[1], "all") == 0)
    {
        STAGE = all;
        ost.open("all_log.txt", ios::out);
    }
    else
    {
        out(ERR_LOG, true);
        return -1;
    }
    TEST_PATH = argv[2];
    if (TEST_PATH.empty() || TEST_PATH.back() != '/') TEST_PATH += '/';
    if (!std::filesystem::exists(TEST_PATH))
    {
        out2("测评路径 " + TEST_PATH + " 不存在\n");
        return -1;
    }
    if (!std::filesystem::is_directory(TEST_PATH))
    {
        out2("测评路径 " + TEST_PATH + " 不是文件夹\n");
        return -1;
    }
    if (std::strcmp(argv[3], "debug") == 0)
        TYPE = debug;
    else if (std::strcmp(argv[3], "test") == 0)
        TYPE = test;
    else
    {
        out2(ERR_LOG);
        return -1;
    }
    return 0;
}

static string pad(int count)
{
    return count > 0 ? std::string(count, ' ') : std::string();
}

static string pad(int count, char target)
{
    return count > 0 ? std::string(count, target) : std::string();
}

static int allmain(int argc, char* argv[])
{
    auto cmd = R"(ls )" + TEST_PATH + R"(*.cminus | sort -V)";
    auto result = runCommand(cmd);
    string flags[3] = {"", "-mem2reg ", "-mem2reg -licm "};
    string tys[3] = {"raw", "mem2reg", "licm"};
    if (result.have_err_message()) out2e(result.err_str);
    auto io_c = runCommand("realpath ../../").out_str;
    io_c.pop_back();
    io_c += "/src/io/io.c";
    auto io_h = io_c;
    io_h.back() = 'h';
    auto tests = splitString(result.out_str);
    string out_path = "./output/";
    out2("[info] Start testing, using testcase dir: " + TEST_PATH + "\n");
    int maxLen = 0;
    for (const auto& line : tests) maxLen = std::max(maxLen, static_cast<int>(line.size()));
    for (const auto& line : tests)
    {
        auto no_path_have_suffix = lastLineRight(line);
        auto no_path_no_suffix = lastDotLeft(no_path_have_suffix);
        makeDir(out_path + no_path_no_suffix);
        auto in_file = TEST_PATH + no_path_no_suffix + ".in";
        auto std_out_file = TEST_PATH + no_path_no_suffix + ".out";
        out2("==========" + no_path_have_suffix + pad(maxLen - static_cast<int>(line.length()), '=') + "==========\n");
        int sz[2] = {};
        for (int i = 0; i < 3; i++)
        {
            const auto& arg = flags[i];
            const auto& ty = tys[i];
            auto ll_file = out_path + no_path_no_suffix + "/" + ty + ".ll";
            auto asm_file = out_path + no_path_no_suffix + "/" + ty + ".s";
            auto exe_file = out_path + no_path_no_suffix + "/" + ty + "o";
            auto out_file = out_path + no_path_no_suffix + "/" + ty + ".out";
            auto eval_file = out_path + no_path_no_suffix + "/eval_" + ty + ".txt";
            out(ty + pad(7 - static_cast<int>(ty.length())) + " ", true);
            out("==========" + ty + pad(7 - static_cast<int>(ty.length()), '=') + "==========\n",
                false);
            cout.flush();
            ost.flush();
            auto cmd2 = runCommandMix("cminusfc -S " + arg + line + " -o " + asm_file);
            out(cmd2.str, false);
            if (cmd2.ret_val)
            {
                out2e("CE: cminusfc compiler .cminus error\n");
                continue;
            }
            if (TYPE == debug)
            {
                cmd2 = runCommandMix(
                    "loongarch64-unknown-linux-gnu-gcc -g -static " + asm_file + " " + io_c + " -o " + exe_file);
                out(cmd2.str, false);
                if (cmd2.ret_val)
                {
                    out2e("CE: gcc compiler .s error\n");
                    continue;
                }
            }
            else
            {
                cmd2 = runCommandMix(
                    "loongarch64-unknown-linux-gnu-gcc -static " + asm_file + " " + io_c + " -o " + exe_file);
                out(cmd2.str, false);
                if (cmd2.ret_val)
                {
                    out2e("CE: gcc compiler .s error\n");
                    continue;
                }
            }
            cmd_result ret;
            if (filesystem::exists(in_file))
                ret = runCommand("qemu-loongarch64 " + exe_file + " >" + out_file + " <" + in_file);
            else ret = runCommand("qemu-loongarch64 " + exe_file + " >" + out_file);
            auto o = readFile(out_file);
            writeFile(o + to_string(ret.ret_val) + "\n", out_file);
            writeFile(ret.err_str, eval_file);
            cmd2 = runCommandMix("diff --strip-trailing-cr " + std_out_file + " " + out_file + " -y");
            out(cmd2.str, false);
            if (cmd2.ret_val != 0)
            {
                out2e("WA: output differ, check " + std_out_file + " and " + out_file + "\n");
                continue;
            }
            out(green(" OK"), true);
            out("OK\n", false);
            auto strs = splitString(ret.err_str);
            if (strs.size() >= 6)
            {
                out(" Take Time (us): " + green(strs.back()) + pad(
                        sz[0] == 0 ? 1 : (sz[0] - static_cast<int>(strs.back().size()))), true);
                out(" Take Time (us): " + strs.back() + pad(
                        sz[0] == 0 ? 1 : (sz[0] - static_cast<int>(strs.back().size()))), false);
                if (sz[0] == 0) sz[0] = static_cast<int>(strs.back().size()) + 1;
                strs.pop_back();
                strs.pop_back();
                out(" Inst Execute Cost: " + green(strs.back()) + pad(sz[1] - static_cast<int>(strs.back().size())),
                    true);
                out(" Inst Execute Cost: " + strs.back() + pad(sz[1] - static_cast<int>(strs.back().size())), false);
                if (sz[1] == 0) sz[1] = static_cast<int>(strs.back().size());
                strs.pop_back();
                strs.pop_back();
                out(" Allocate Size (bytes): " + green(strs.back()) + "\n", true);
                out(" Allocate Size (bytes): " + strs.back() + "\n", false);
            }
            else cout << "\n";
            filesystem::remove(exe_file);
            filesystem::remove(out_file);
            filesystem::remove(asm_file);
        }
    }
    return 0;
}

int main(int argc, char* argv[])
{
    if (parseCmd(argc, argv)) ext(-1);
    if (STAGE == all)
    {
        return allmain(argc, argv);
    }
    auto cmd = R"(ls )" + TEST_PATH + R"(*.cminus | sort -V)";
    auto result = runCommand(cmd);
    string flags = (STAGE == mem2reg ? "-mem2reg " : (STAGE == raw ? "" : "-mem2reg -licm "));
    if (result.have_err_message()) out2e(result.err_str);
    auto io_c = runCommand("realpath ../../").out_str;
    io_c.pop_back();
    io_c += "/src/io/io.c";
    auto io_h = io_c;
    io_h.back() = 'h';
    auto tests = splitString(result.out_str);
    string out_path = "./output/";
    out2("[info] Start testing, using testcase dir: " + TEST_PATH + "\n");
    int maxLen = 0;
    for (const auto& line : tests) maxLen = std::max(maxLen, static_cast<int>(line.size()));
    for (const auto& line : tests)
    {
        auto no_path_have_suffix = lastLineRight(line);
        auto no_path_no_suffix = lastDotLeft(no_path_have_suffix);
        makeDir(out_path + no_path_no_suffix);
        auto ll_file = out_path + no_path_no_suffix + "/" + argv[1] + ".ll";
        auto asm_file = out_path + no_path_no_suffix + "/" + argv[1] + ".s";
        auto exe_file = out_path + no_path_no_suffix + "/" + argv[1] + "o";
        auto out_file = out_path + no_path_no_suffix + "/" + argv[1] + ".out";
        auto eval_file = out_path + no_path_no_suffix + "/eval_" + argv[1] + ".txt";
        auto in_file = TEST_PATH + no_path_no_suffix + ".in";
        auto std_out_file = TEST_PATH + no_path_no_suffix + ".out";
        out(no_path_have_suffix + pad(maxLen - static_cast<int>(line.length())) + " ", true);
        out("==========" + no_path_have_suffix + pad(maxLen - static_cast<int>(line.length()), '=') + "==========\n",
            false);
        cout.flush();
        ost.flush();
        auto cmd2 = runCommandMix("cminusfc -S " + flags + line + " -o " + asm_file);
        out(cmd2.str, false);
        if (cmd2.ret_val)
        {
            out2e("CE: cminusfc compiler .cminus error\n");
            continue;
        }
        if (TYPE == debug)
        {
            cmd2 = runCommandMix(
                "loongarch64-unknown-linux-gnu-gcc -g -static " + asm_file + " " + io_c + " -o " + exe_file);
            out(cmd2.str, false);
            if (cmd2.ret_val)
            {
                out2e("CE: gcc compiler .s error\n");
                continue;
            }
        }
        else
        {
            cmd2 = runCommandMix(
                "loongarch64-unknown-linux-gnu-gcc -static " + asm_file + " " + io_c + " -o " + exe_file);
            out(cmd2.str, false);
            if (cmd2.ret_val)
            {
                out2e("CE: gcc compiler .s error\n");
                continue;
            }
        }
        cmd_result ret;
        if (filesystem::exists(in_file))
            ret = runCommand("qemu-loongarch64 " + exe_file + " >" + out_file + " <" + in_file);
        else ret = runCommand("qemu-loongarch64 " + exe_file + " >" + out_file);
        auto o = readFile(out_file);
        writeFile(o + to_string(ret.ret_val) + "\n", out_file);
        writeFile(ret.err_str, eval_file);
        cmd2 = runCommandMix("diff --strip-trailing-cr " + std_out_file + " " + out_file + " -y");
        out(cmd2.str, false);
        if (cmd2.ret_val != 0)
        {
            out2e("WA: output differ, check " + std_out_file + " and " + out_file + "\n");
            continue;
        }
        out(green(" OK"), true);
        out("OK\n", false);
        auto strs = splitString(ret.err_str);
        if (strs.size() >= 6)
        {
            out(" Take Time (us): " + green(strs.back()), true);
            out(" Take Time (us): " + strs.back(), false);
            strs.pop_back();
            strs.pop_back();
            out(" Inst Execute Cost: " + green(strs.back()), true);
            out(" Inst Execute Cost: " + strs.back(), false);
            strs.pop_back();
            strs.pop_back();
            out(" Allocate Size (bytes): " + green(strs.back()) + "\n", true);
            out(" Allocate Size (bytes): " + strs.back() + "\n", false);
        }
        else cout << "\n";
        filesystem::remove(exe_file);
        filesystem::remove(out_file);
        filesystem::remove(asm_file);
    }
}
