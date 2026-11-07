#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/personality.h>
#include <sys/user.h>
#include <unistd.h>
#include <elf.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>
using namespace std;

struct Sym { uintptr_t addr; uint64_t size; };

class Breakpoint {
public:
    Breakpoint() : pid_(0), addr_(0), saved_(0), enabled_(false) {}
    Breakpoint(pid_t pid, uintptr_t addr)
        : pid_(pid), addr_(addr), saved_(0), enabled_(false) {}

    bool enable() {
        if (enabled_) return true;
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, pid_, (void*)addr_, nullptr);
        if (errno) return false;                      // bad address
        saved_ = (uint8_t)(word & 0xff);              // remember original byte
        long patched = (word & ~0xffL) | 0xcc;        // put INT3 in its place
        if (ptrace(PTRACE_POKEDATA, pid_, (void*)addr_, (void*)patched) == -1)
            return false;
        enabled_ = true;
        return true;
    }
    void disable() {
        if (!enabled_) return;
        long word = ptrace(PTRACE_PEEKDATA, pid_, (void*)addr_, nullptr);
        long restored = (word & ~0xffL) | saved_;
        ptrace(PTRACE_POKEDATA, pid_, (void*)addr_, (void*)restored);
        enabled_ = false;
    }
    bool enabled() const { return enabled_; }
private:
    pid_t pid_;
    uintptr_t addr_;
    uint8_t saved_;
    bool enabled_;
};

class Debugger {
public:
    explicit Debugger(string prog)
        : prog_(prog), pid_(0), base_(0), isPie_(false), running_(false) {}

    void run() {
        pid_ = fork();
        if (pid_ == 0) {                                  // child
            personality(ADDR_NO_RANDOMIZE);               // stable addresses
            ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);  // "my parent debugs me"
            execl(prog_.c_str(), prog_.c_str(), (char*)nullptr);
            perror("execl");
            _exit(127);
        }
        int status;                                       // parent
        waitpid(pid_, &status, 0);                        // stops right after exec
        if (!WIFSTOPPED(status)) { cerr << "could not start " << prog_ << "\n"; return; }
        running_ = true;
        ptrace(PTRACE_SETOPTIONS, pid_, nullptr, (void*)PTRACE_O_EXITKILL);
        loadSymbols();
        cout << "Started " << prog_ << " (pid " << pid_ << "), "
             << symbols_.size() << " function symbols\n";
        repl();
    }

private:
    // ---------- ELF symbols ----------
    void loadSymbols() {
        ifstream f(prog_, ios::binary);
        vector<char> buf((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
        if (buf.size() < sizeof(Elf64_Ehdr)) return;
        auto* eh = reinterpret_cast<Elf64_Ehdr*>(buf.data());
        isPie_ = (eh->e_type == ET_DYN);
        auto* sh = reinterpret_cast<Elf64_Shdr*>(buf.data() + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; i++) {
            if (sh[i].sh_type != SHT_SYMTAB) continue;
            auto* syms = reinterpret_cast<Elf64_Sym*>(buf.data() + sh[i].sh_offset);
            size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
            const char* strtab = buf.data() + sh[sh[i].sh_link].sh_offset;
            for (size_t k = 0; k < n; k++) {
                if (ELF64_ST_TYPE(syms[k].st_info) == STT_FUNC && syms[k].st_value != 0)
                    symbols_[string(strtab + syms[k].st_name)] =
                        Sym{syms[k].st_value, syms[k].st_size};
            }
        }
        if (isPie_) base_ = loadBase();
    }

    uintptr_t loadBase() {                                // first mapping of our binary
        char resolved[4096];
        if (!realpath(prog_.c_str(), resolved)) return 0;
        ifstream maps("/proc/" + to_string(pid_) + "/maps");
        string line;
        while (getline(maps, line))
            if (line.find(resolved) != string::npos)
                return stoull(line.substr(0, line.find('-')), nullptr, 16);
        return 0;
    }

    string symbolFor(uintptr_t pc) {
        uintptr_t rel = pc - base_;
        for (auto& [name, s] : symbols_)
            if (rel >= s.addr && rel < s.addr + s.size) {
                char buf[32];
                snprintf(buf, sizeof buf, "+0x%lx", (unsigned long)(rel - s.addr));
                return name + buf;
            }
        return "";
    }

    // ---------- registers ----------
    user_regs_struct getRegs() {
        user_regs_struct r;
        ptrace(PTRACE_GETREGS, pid_, nullptr, &r);
        return r;
    }
    void setRegs(const user_regs_struct& r) { ptrace(PTRACE_SETREGS, pid_, nullptr, &r); }

    // ---------- waiting for the target ----------
    // wasStep = true  -> the stop came from a single-step (no INT3 to rewind)
    // returns false if the target is gone
    bool waitStop(bool wasStep) {
        int status;
        waitpid(pid_, &status, 0);
        if (WIFEXITED(status)) {
            cout << "Program exited with code " << WEXITSTATUS(status) << "\n";
            running_ = false; return false;
        }
        if (WIFSIGNALED(status)) {
            cout << "Program killed by signal " << WTERMSIG(status) << "\n";
            running_ = false; return false;
        }
        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);
            if (sig == SIGTRAP && !wasStep) {
                user_regs_struct r = getRegs();
                uintptr_t pc = r.rip - 1;                 // rip is one past the 0xCC
                auto it = bps_.find(pc);
                if (it != bps_.end() && it->second.enabled()) {
                    r.rip = pc;                           // rewind onto the breakpoint
                    setRegs(r);
                    cout << "Breakpoint hit at 0x" << hex << pc << dec << "\n";
                }
            } else if (sig != SIGTRAP) {
                cout << "Stopped by signal " << sig << " (" << strsignal(sig) << ")\n";
            }
        }
        return true;
    }

    // execute exactly one instruction (stepping off a breakpoint if we sit on one)
    bool singleStep() {
        uintptr_t pc = getRegs().rip;
        auto it = bps_.find(pc);
        bool onBp = (it != bps_.end() && it->second.enabled());
        if (onBp) it->second.disable();                   // restore original byte
        ptrace(PTRACE_SINGLESTEP, pid_, nullptr, nullptr);
        bool alive = waitStop(true);
        if (alive && onBp) it->second.enable();           // put 0xCC back
        return alive;
    }

    void showLocation() {
        uintptr_t pc = getRegs().rip;
        cout << "pc = 0x" << hex << pc << dec;
        string s = symbolFor(pc);
        if (!s.empty()) cout << "  <" << s << ">";
        cout << "\n";
    }

    // ---------- commands ----------
    void cmdContinue() {
        if (!running_) return;
        uintptr_t pc = getRegs().rip;
        auto it = bps_.find(pc);
        if (it != bps_.end() && it->second.enabled())
            if (!singleStep()) return;                    // step off the breakpoint first
        ptrace(PTRACE_CONT, pid_, nullptr, nullptr);
        waitStop(false);
        if (running_) showLocation();
    }

    void cmdStep() {
        if (!running_) return;
        if (singleStep()) showLocation();
    }

    void cmdBreak(const string& arg) {
        uintptr_t addr;
        if (arg[0] == '*') {
            addr = stoull(arg.substr(1), nullptr, 0);     // break *0xADDR
        } else {
            auto it = symbols_.find(arg);                 // break main
            if (it == symbols_.end()) { cout << "No symbol '" << arg << "'\n"; return; }
            addr = it->second.addr + base_;
        }
        if (bps_.count(addr)) { cout << "Already set\n"; return; }
        Breakpoint bp(pid_, addr);
        if (!bp.enable()) { cout << "Cannot write to 0x" << hex << addr << dec << "\n"; return; }
        bps_[addr] = bp;
        cout << "Breakpoint at 0x" << hex << addr << dec << "\n";
    }

    void cmdRegs() {
        user_regs_struct r = getRegs();
        printf("rip=%016llx rsp=%016llx rbp=%016llx\n", r.rip, r.rsp, r.rbp);
        printf("rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n", r.rax, r.rbx, r.rcx, r.rdx);
        printf("rsi=%016llx rdi=%016llx r8 =%016llx r9 =%016llx\n", r.rsi, r.rdi, r.r8, r.r9);
        printf("eflags=%016llx\n", r.eflags);
    }

    void cmdExamine(const string& a, int count) {          // x 0xADDR [count]
        uintptr_t addr = stoull(a, nullptr, 0);
        for (int i = 0; i < count; i++) {
            errno = 0;
            long w = ptrace(PTRACE_PEEKDATA, pid_, (void*)(addr + i * 8), nullptr);
            if (errno) { cout << "Cannot read memory there\n"; return; }
            printf("0x%016lx: 0x%016lx\n", (unsigned long)(addr + i * 8), (unsigned long)w);
        }
    }

    void repl() {
        string line;
        while (running_) {
            cout << "(minidbg) " << flush;
            if (!getline(cin, line)) break;
            istringstream in(line);
            string cmd, a1, a2;
            in >> cmd >> a1 >> a2;
            if (cmd.empty()) continue;
            try {
                if (cmd == "q" || cmd == "quit") break;
                else if (cmd == "c" || cmd == "continue") cmdContinue();
                else if (cmd == "s" || cmd == "step") cmdStep();
                else if (cmd == "b" || cmd == "break") {
                    if (a1.empty()) cout << "usage: break <function> | break *0xADDR\n";
                    else cmdBreak(a1);
                }
                else if (cmd == "regs") cmdRegs();
                else if (cmd == "x" && !a1.empty()) cmdExamine(a1, a2.empty() ? 1 : stoi(a2));
                else cout << "commands: break, continue, step, regs, x <addr> [n], quit\n";
            } catch (const exception&) {
                cout << "bad argument\n";
            }
        }
        if (running_) { kill(pid_, SIGKILL); waitpid(pid_, nullptr, 0); }
    }

    string prog_;
    pid_t pid_;
    uintptr_t base_;
    bool isPie_, running_;
    map<string, Sym> symbols_;
    map<uintptr_t, Breakpoint> bps_;
};

int main(int argc, char** argv) {
    if (argc < 2) { cerr << "usage: " << argv[0] << " <program>\n"; return 1; }
    Debugger dbg(argv[1]);
    dbg.run();
    return 0;
}
