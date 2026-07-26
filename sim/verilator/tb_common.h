// sim/verilator/tb_common.h ---------------------------------------------------
#pragma once
#include <verilated.h>
#include <verilated_fst_c.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

// ---- global pass/fail state -------------------------------------------------
inline int      tb_checks   = 0;
inline int      tb_fails    = 0;
inline int      tb_reported = 0;
inline const int TB_MAX_REPORT = 25;   // stop spamming after N failures
inline std::string tb_section = "init";

inline void tb_begin(const char* name) {
    tb_section = name;
    printf("--- %s\n", name);
}

inline void tb_check(const char* file, int line, const char* expr,
                     uint64_t got, uint64_t want) {
    tb_checks++;
    if (got == want) return;
    tb_fails++;
    if (tb_reported++ < TB_MAX_REPORT) {
        printf("FAIL [%s] %s:%d  %s\n"
               "     got  0x%08llx (%llu)\n"
               "     want 0x%08llx (%llu)\n",
               tb_section.c_str(), file, line, expr,
               (unsigned long long)got,  (unsigned long long)got,
               (unsigned long long)want, (unsigned long long)want);
    }
}

#define CHECK_EQ(got, want) \
    tb_check(__FILE__, __LINE__, #got, (uint64_t)(got), (uint64_t)(want))

// ---- DUT wrapper ------------------------------------------------------------
// DUT = the Verilator-generated class (Vregfile, Valu, ...).
// tick() is only compiled if you call it, so combinational DUTs with no clk
// port still work with this same template.
template <class DUT>
class Tb {
public:
    VerilatedContext ctx;
    DUT              dut;
    VerilatedFstC    tfp;
    uint64_t         t    = 0;      // ps
    uint64_t         half = 5000;   // 5 ns half-period -> 100 MHz
    std::mt19937     rng;
    uint32_t         seed = 1;

    Tb(int argc, char** argv, const std::string& name)
        : dut(&ctx, "TOP")
    {
        ctx.commandArgs(argc, argv);
        for (int i = 1; i < argc; i++)
            if (!strncmp(argv[i], "+seed=", 6)) seed = strtoul(argv[i] + 6, nullptr, 0);
        rng.seed(seed);

        ctx.traceEverOn(true);
        dut.trace(&tfp, 99);
        tfp.open((name + ".fst").c_str());
        printf("[tb] %s  seed=%u  wave=%s.fst\n", name.c_str(), seed, name.c_str());
    }

    ~Tb() { tfp.close(); }

    void dump()  { tfp.dump(t); }
    void eval()  { dut.eval(); dump(); }

    // Combinational DUTs: apply inputs, then call settle().
    void settle(uint64_t ps = 1000) { dut.eval(); dump(); t += ps; dut.eval(); dump(); }

    // Clocked DUTs: apply inputs (clk is low), then tick(). Leaves clk low.
    void tick() {
        dut.eval();          dump();   // settle inputs before the edge
        t += half;
        dut.clk = 1; dut.eval(); dump();   // rising edge
        t += half;
        dut.clk = 0; dut.eval(); dump();   // falling edge
    }

    uint32_t rnd()               { return (uint32_t)rng(); }
    uint32_t rnd(uint32_t lo, uint32_t hi) { return lo + rng() % (hi - lo + 1); }

    int finish() {
        t += half; dump();
        dut.final();
        printf("[tb] %d checks, %d failures -> %s\n",
               tb_checks, tb_fails, tb_fails ? "FAIL" : "PASS");
        return tb_fails ? 1 : 0;
    }
};
