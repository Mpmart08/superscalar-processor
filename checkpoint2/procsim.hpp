#ifndef PROCSIM_HPP
#define PROCSIM_HPP

#include <cstdint>
#include <cstdio>

#define DEFAULT_K0 3
#define DEFAULT_K1 2
#define DEFAULT_K2 1
#define DEFAULT_R 2
#define DEFAULT_F 4
#define DEFAULT_E 250
#define DEFAULT_S 0

enum class State {FETCHED, DISPATCHED, FIRED, EXECUTED, COMPLETED, RETIRED};

typedef void (*FP)();

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
    bool exception = false;

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
    
    unsigned long reg_file_hit_count;
    unsigned long rob_hit_count;
    unsigned long exception_count;
    unsigned long backup_count;
    unsigned long flushed_count;
} proc_stats_t;

bool read_instruction(proc_inst_t* p_inst);

void setup_proc(uint64_t r, uint64_t k0, uint64_t k1, uint64_t k2, uint64_t f, uint64_t e, uint64_t s);
void run_proc(proc_stats_t* p_stats);
void complete_proc(proc_stats_t* p_stats);
void print_instructions();

void cycle_stage_0();
void cycle_stage_0_rob();
void cycle_stage_0_cpr();
void cycle_stage_1();
void cycle_stage_1_rob();
void cycle_stage_1_cpr();
void cycle_stage_2();
void cycle_stage_2_rob();
void cycle_stage_2_cpr();
void cycle_stage_3();
void cycle_stage_3_rob();
void cycle_stage_3_cpr();
void cycle_stage_4();
void cycle_stage_4_rob();
void cycle_stage_4_cpr();
void cycle_stage_5();
void cycle_stage_5_rob();
void cycle_stage_5_cpr();
void cycle_stage_6();
void cycle_stage_6_rob();
void cycle_stage_6_cpr();

#endif /* PROCSIM_HPP */
