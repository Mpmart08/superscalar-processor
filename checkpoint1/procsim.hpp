#ifndef PROCSIM_HPP
#define PROCSIM_HPP

#include <cstdint>
#include <cstdio>

#define DEFAULT_K0 3
#define DEFAULT_K1 2
#define DEFAULT_K2 1
#define DEFAULT_R 2
#define DEFAULT_F 4

enum class State {FETCHED, DISPATCHED, FIRED, EXECUTED, COMPLETED, RETIRED};

typedef struct _reg_t
{
    bool ready = true;
    uint32_t tag;

} reg_t;

typedef struct _proc_inst_t
{
    uint32_t instruction_address;
    uint32_t inst_tag;
    int32_t op_code;
    uint32_t fu;
    int32_t dest_reg;
    uint32_t dest_tag;
    int32_t src_reg[2];
    uint32_t src_tag[2];
    bool src_ready[2];
    unsigned long fired_cycle;
    State state;

    uint32_t fetch;
    uint32_t disp;
    uint32_t sched;
    uint32_t exec;
    uint32_t update;
    
} proc_inst_t;

typedef struct _proc_stats_t
{
    float avg_inst_retired;
    float avg_inst_fired;
    float avg_disp_size;
    unsigned long max_disp_size;
    unsigned long retired_instruction;
    unsigned long cycle_count;

} proc_stats_t;

bool read_instruction(proc_inst_t* p_inst);

void setup_proc(uint64_t r, uint64_t k0, uint64_t k1, uint64_t k2, uint64_t f);
void run_proc(proc_stats_t* p_stats);
void complete_proc(proc_stats_t* p_stats);
void print_statistics(proc_stats_t* p_stats);
void print_instructions();

#endif /* PROCSIM_HPP */
