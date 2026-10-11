
#include "Module.hpp"
#include "PassManager.hpp"
#include "ast.hpp"
#include "cminusf_builder.hpp"
#include "CodeGen.hpp"
#include "MachineCodeGen.hpp"
#include "PassManager.hpp"
#include "DeadCode.hpp"
#include "Mem2Reg.hpp"
#include "LoopDetection.hpp"
#include "LICM.hpp"
#include "ConstPropagation.hpp"
#include "GVN.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using std::string;
using std::operator""s;

struct Config {
    string exe_name; // compiler exe name
    std::filesystem::path input_file;
    std::filesystem::path output_file;

    bool emitast{ false };
    bool emitasm{ false };
    bool emitllvm{ false };
    // optization conifg
    bool mem2reg{ false };
    bool licm{ false };
    bool const_prop{ false };
    bool gvn{ false };
    bool dump_json{ false };
    string backend{ "legacy" };
    string isel{ "baseline" };
    bool backend_explicit{ false };
    bool isel_explicit{ false };
    bool dump_mir{ false };
    bool lab4_metrics{ false };

    Config(int argc, char** argv) : argc(argc), argv(argv) {
        parse_cmd_line();
        check();
    }

private:
    int argc{ -1 };
    char** argv{ nullptr };

    void parse_cmd_line();
    void check();
    // print helper infomation and exit
    void print_help() const;
    void print_err(const string& msg) const;
};

int main(int argc, char** argv) {
    Config config(argc, argv);

    auto syntax_tree = parse(config.input_file.c_str());
    if (syntax_tree == nullptr) {
        // 语法错误的具体位置已经由 yyerror 打到 stderr 了, 这里只补一句收束并干净退出。
        // 注意必须在这里拦住: AST 的构造函数拿到 nullptr 会 std::abort(), 消息还会误导成
        // "empty input tree!"
        std::cerr << "[ERR] syntax analysis failed, no output generated.\n";
        return 1;
    }
    auto ast = AST(syntax_tree);

    if (config.emitast) { // if emit ast (lab1), print ast and return
        ASTPrinter printer;
        ast.run_visitor(printer);
    }
    else {
        Module* m;
        CminusfBuilder builder;
        ast.run_visitor(builder);
        m = builder.getModule();

        PassManager PM(m);
        // optimization 
        if (config.mem2reg) {
            PM.add_pass<DeadCode>(true);
            PM.add_pass<Mem2Reg>();
            PM.add_pass<DeadCode>(false);
        }
        if (config.const_prop) {
            PM.add_pass<ConstPropagation>();
            PM.add_pass<DeadCode>(false);
        }
        if (config.gvn) {
            PM.add_pass<GVN>(config.dump_json);
            PM.add_pass<DeadCode>(false);
        }
        if (config.licm) {
            PM.add_pass<LoopInvariantCodeMotion>();
            PM.add_pass<DeadCode>(false);
        }
        try {
            PM.run();
        } catch (const std::exception &error) {
            std::cerr << "[ERR] " << error.what() << '\n';
            delete m;
            return 1;
        }

        if (config.emitasm && config.backend == "mir") {
            try {
                machine::Options options;
                options.selection = config.isel == "greedy" ? machine::SelectionMode::Greedy
                                                             : machine::SelectionMode::Baseline;
                options.lab4_metrics = config.lab4_metrics;
                auto result = machine::generate(*m, options);
                // Complete all stages before opening artifacts, including the LLVM sidecar.
                auto write = [](const std::filesystem::path &path, const string &text) {
                    std::ofstream stream(path);
                    stream << text;
                    stream.close();
                    if (!stream)
                        throw std::runtime_error("cannot write backend output: " + path.string());
                };
                write(config.output_file, result.assembly);
                auto path = config.output_file;
                path.replace_extension(".ll");
                write(path, m->print());
                if (config.dump_mir) {
                    path.replace_extension(".selected.mir");
                    write(path, result.selected);
                    path = config.output_file;
                    path.replace_extension(".prera.mir");
                    write(path, result.prera);
                    path = config.output_file;
                    path.replace_extension(".final.mir");
                    write(path, result.final);
                }
            } catch (const std::exception &error) {
                std::cerr << "[ERR] " << error.what() << '\n';
                delete m;
                return 1;
            }
            delete m;
            return 0;
        }

        std::ofstream output_stream(config.output_file);
        if (config.emitllvm) {
            auto abs_path = std::filesystem::canonical(config.input_file);
            output_stream << "; ModuleID = 'cminus'\n";
            output_stream << "source_filename = " << abs_path << "\n\n";
            output_stream << m->print();
        }
        else if (config.emitasm) {
            auto abs_path = std::filesystem::canonical(config.input_file);
            config.output_file.replace_extension("ll");
            std::ofstream output_stream2(config.output_file);
            output_stream2 << "; ModuleID = 'cminus'\n";
            output_stream2 << "source_filename = " << abs_path << "\n\n";
            output_stream2 << m->print();
            CodeGen codegen(m);
            codegen.run();
            output_stream << codegen.print();
        }

        delete m;
    }

    return 0;
}

void Config::parse_cmd_line() {
    exe_name = argv[0];
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == "-h"s || argv[i] == "--help"s) {
            print_help();
        }
        else if (argv[i] == "-o"s) {
            if (output_file.empty() && i + 1 < argc) {
                output_file = argv[i + 1];
                i += 1;
            }
            else {
                print_err("bad output file");
            }
        }
        else if (argv[i] == "-emit-ast"s) {
            emitast = true;
        }
        else if (argv[i] == "-S"s) {
            emitasm = true;
        }
        else if (argv[i] == "-emit-llvm"s) {
            emitllvm = true;
        }
        else if (argv[i] == "-mem2reg"s) {
            mem2reg = true;
        }
        else if (argv[i] == "-licm"s) {
            licm = true;
        }
        else if (argv[i] == "-const-prop"s) {
            const_prop = true;
        }
        else if (argv[i] == "-gvn"s) {
            gvn = true;
        }
        else if (argv[i] == "-dump-json"s) {
            dump_json = true;
        }
        else if (string(argv[i]).rfind("-backend=", 0) == 0) {
            backend = string(argv[i]).substr(9);
            backend_explicit = true;
        }
        else if (string(argv[i]).rfind("-isel=", 0) == 0) {
            isel = string(argv[i]).substr(6);
            isel_explicit = true;
        }
        else if (argv[i] == "-dump-mir"s) {
            dump_mir = true;
        }
        else if (argv[i] == "-lab4-metrics"s) {
            lab4_metrics = true;
        }
        else {
            if (input_file.empty()) {
                input_file = argv[i];
            }
            else {
                string err =
                    "unrecognized command-line option \'"s + argv[i] + "\'"s;
                print_err(err);
            }
        }
    }
}

void Config::check() {
    if (input_file.empty()) {
        print_err("no input file");
    }
    if (input_file.extension() != ".cminus") {
        print_err("file format not recognized");
    }
    if (emitllvm and emitasm) {
        print_err("emit llvm and emit asm both set");
    }
    if (backend != "legacy" && backend != "mir") {
        print_err("unknown backend: " + backend);
    }
    if (isel != "baseline" && isel != "greedy") {
        print_err("unknown instruction selection mode: " + isel);
    }
    if ((backend_explicit || isel_explicit || dump_mir || lab4_metrics) && (!emitasm || emitast)) {
        print_err("backend options require -S");
    }
    if (backend != "mir" && (isel_explicit || dump_mir || lab4_metrics)) {
        print_err("isel, dump-mir and lab4-metrics require backend=mir");
    }
    if (not emitllvm and not emitasm and not emitast) {
        print_err("not supported: generate executable file directly");
    }
    if (licm and not mem2reg) {
        print_err("licm must be used with mem2reg");
    }
    if (gvn and not mem2reg) {
        print_err("gvn must be used with mem2reg");
    }
    if (dump_json and not gvn) {
        print_err("dump-json must be used with gvn");
    }
    if (emitast and (const_prop or gvn or dump_json)) {
        print_err("lab5 optimization options cannot be used with emit-ast");
    }
    if (output_file.empty()) {
        output_file = input_file.stem();
    }
    if (emitllvm) {
        output_file.replace_extension(".ll");
    }
    else if (emitasm) {
        output_file.replace_extension(".s");
    }
}

void Config::print_help() const {
    std::cout << "Usage: " << exe_name
        << " [-h|--help] [-o <target-file>] [-emit-llvm|-S|-emit-ast]"
        " [-mem2reg] [-const-prop] [-gvn] [-dump-json] [-licm]"
        " [-backend=legacy|mir] [-isel=baseline|greedy] [-dump-mir] [-lab4-metrics]"
        " <input-file>"
        << std::endl;
    exit(0);
}

void Config::print_err(const string& msg) const {
    std::cout << exe_name << ": " << msg << std::endl;
    exit(-1);
}
