#include "procsim.hpp"
#include <cmath>
#include <list>
#include <iterator>
#include <fstream>

using namespace std;

//==================//
// Global Variables //
//==================//

// log file
ofstream log_file;

// instructions
list<proc_inst_t*> instructions;

// register file
reg_t* reg;

// scoreboard of function units
list<proc_inst_t*> sb;

// result buses
proc_inst_t** cdb;

// dispatch queue
list<proc_inst_t*> dq;
unsigned long dq_size = 0;
unsigned long dq_max_size = 0;
unsigned long dq_size_sum = 0;

// scheduling queue
list<proc_inst_t*> sq;
unsigned long sq_size = 0;
unsigned long sq_max_size;

// processor parameters
uint64_t r;
uint64_t k[3];
uint64_t f;

// counters
unsigned long inst_tag_counter = 1;
unsigned long reg_tag_counter = 128;
unsigned long cycle_counter = 1;
unsigned long retired_counter = 1;
unsigned long fu_busy_counter[3] = {0, 0, 0};

//==============//
// Cycle Stages //
//==============//

// mark completed intstructions as retired
void cycle_stage_0()
{
    for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end(); ++iterator) {

        proc_inst_t* inst = *iterator;
        if (inst->state == State::COMPLETED) {
            inst->state = State::RETIRED;

            char log_line[80];
            sprintf(log_line, "%lu\tSTATE UPDATE\t%u\n", cycle_counter, inst->inst_tag);
            log_file << log_line;

            inst->update = cycle_counter;
        }
    }
}

// broadcast results on result buses
// mark instructions as completed
// update register files
void cycle_stage_1()
{
    sb.sort([](proc_inst_t* x, proc_inst_t* y) {
        if (x->fired_cycle < y->fired_cycle) {
            return true;
        } else if (x->fired_cycle > y->fired_cycle) {
            return false;
        } else {
            return x->inst_tag < y->inst_tag;
        }
    });

    unsigned long buses_used = 0;

    // if all cdbs are used
    // or there are no instructions waiting to be broadcast
    // then exit loop
    for (list<proc_inst_t*>::iterator iterator = sb.begin(); iterator != sb.end();) {

        proc_inst_t* inst = *iterator;

        // there is a free bus
        if (buses_used < r) {
            // put results on bus and free FU
            cdb[buses_used] = inst;
            buses_used++;
            fu_busy_counter[inst->fu]--;

            if (inst->state != State::EXECUTED) {
                inst->state = State::EXECUTED;

                char log_line[80];
                sprintf(log_line, "%lu\tEXECUTED\t%u\n", cycle_counter, inst->inst_tag);
                log_file << log_line;
            }

            iterator = sb.erase(iterator);

        } else if (inst->state != State::EXECUTED) {

            inst->state = State::EXECUTED;
            
            char log_line[80];
            sprintf(log_line, "%lu\tEXECUTED\t%u\n", cycle_counter, inst->inst_tag);
            log_file << log_line;

            ++iterator;

        } else {

            ++iterator;
        }
    }

    // set tags of unused buses to infinity to prevent them from updating things
    for (unsigned long i = buses_used; i < r; ++i) {
        cdb[i] = new proc_inst_t;
        cdb[i]->dest_tag = UINT32_MAX;
        cdb[i]->inst_tag = UINT32_MAX;
    }

    // mark instruction as completed in scheduling queue
    for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end(); ++iterator) {

        proc_inst_t* inst = *iterator;

        // check each result bus
        for (unsigned long j = 0; j < r; ++j) {
            if (inst->state == State::EXECUTED
                && inst->inst_tag == cdb[j]->inst_tag) {

                inst->state = State::COMPLETED;
                break;
            }
        }
    }

    // update register file
    for (unsigned long j = 0; j < r; ++j) {
        for (int i = 0; i < 128; ++i) {

            if (reg[i].tag == cdb[j]->dest_tag) {
                reg[i].ready = true;
                break;
            }
        }
    }
}

// fire instructions in the scheduling queue
void cycle_stage_2()
{
    for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end(); ++iterator) {

        proc_inst_t* inst = *iterator;

        if (inst->src_ready[0]
            && inst->src_ready[1]
            && fu_busy_counter[inst->fu] < k[inst->fu]
            && inst->state == State::DISPATCHED) {

            inst->state = State::FIRED;
            inst->fired_cycle = cycle_counter;
            sb.push_back(inst);
            fu_busy_counter[inst->fu]++;

            char log_line[80];
            sprintf(log_line, "%lu\tSCHEDULED\t%u\n", cycle_counter, inst->inst_tag);
            log_file << log_line;

            inst->exec = cycle_counter + 1;
        }
    }
}

// update scheduling queue via result buses
void cycle_stage_3()
{
    // for each result bus
    for (unsigned long j = 0; j < r; ++j) {
        // for each instruction
        for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end(); ++iterator) {
            // for each source register
            for (unsigned long k = 0; k < 2; ++k) {

                proc_inst_t* inst = *iterator;

                /*if (inst->inst_tag == 90 && cdb[1]->inst_tag == 88) {
                    printf("\n90: src_tag[0]=%u, src_tag[1]=%u, src_ready[0]=%d, src_ready[1]=%d\n", inst->src_tag[0], inst->src_tag[1], inst->src_ready[0], inst->src_ready[1]);
                    printf("cdb[1]=%u\n\n", cdb[1]->dest_tag);
                }*/

                if (inst->state == State::DISPATCHED
                    && cdb[j]->dest_tag == inst->src_tag[k]) {

                    inst->src_ready[k] = true;
                }
            }
        }
    }
}

// dispatch instructions to scheduling queue
// dispatch queue reads register file
void cycle_stage_4()
{
    // if the scheduling queue is full
    // or the dispatch queue is empty
    // then exit loop
    for (list<proc_inst_t*>::iterator iterator = dq.begin(); iterator != dq.end();) {

        // scheduling queue is full
        if (sq_size == sq_max_size) {
            break;
        }

        proc_inst_t* inst = *iterator;
        iterator = dq.erase(iterator);
        dq_size--;

        char log_line[80];
        sprintf(log_line, "%lu\tDISPATCHED\t%u\n", cycle_counter, inst->inst_tag);
        log_file << log_line;

        inst->sched = cycle_counter + 1;

        inst->state = State::DISPATCHED;

        for (int i = 0; i < 2; ++i) {

            if (inst->src_reg[i] == -1) {

                inst->src_ready[i] = true;

            } else if (reg[inst->src_reg[i]].ready) {

                inst->src_ready[i] = true;

            } else {

                inst->src_tag[i] = reg[inst->src_reg[i]].tag;
                inst->src_ready[i] = false;
            }
        }

        // assign new tag to destination register
        if (inst->dest_reg > -1) {

            reg[inst->dest_reg].tag = reg_tag_counter;
            reg[inst->dest_reg].ready = false;
            inst->dest_tag = reg_tag_counter;
            reg_tag_counter++;
        }

        sq.push_back(inst);
        sq_size++;
    }

    // dispatch queue reads register file
    /*for (list<proc_inst_t>::iterator iterator = dq.begin(); iterator != dq.end(); ++iterator) {

        // set instruction ready bits and tags
        for (int i = 0; i < 2; ++i) {

            if (iterator->src_reg[i] == -1) {

                iterator->src_ready[i] = true;

            } else if (reg[iterator->src_reg[i]].ready) {

                iterator->src_ready[i] = true;

            } else {

                iterator->src_tag[i] = reg[iterator->src_reg[i]].tag;
                iterator->src_ready[i] = false;
            }
        }

        // assign new tag to destination register
        if (iterator->dest_reg == -1) {

            iterator->dest_tag = UINT32_MAX;

        } else {

            reg[iterator->dest_reg].tag = reg_tag_counter;
            reg[iterator->dest_reg].ready = false;
            iterator->dest_tag = reg_tag_counter;
            reg_tag_counter++;
        }
    }*/

    // scheduling queue reads register file
    for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end(); ++iterator) {

        proc_inst_t* inst = *iterator;

        if (inst->state == State::DISPATCHED) {
            // set instruction ready bits
            for (int i = 0; i < 2; ++i) {

                if (inst->src_ready[i] == false) {
                    if (reg[inst->src_reg[i]].tag == inst->src_tag[i] && reg[inst->src_reg[i]].ready) {

                        inst->src_ready[i] = true;
                    }
                }
            }
        }
    }
}

// deletes retired instructions from the scheduling queue
void cycle_stage_5()
{
    for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end();) {

        proc_inst_t* inst = *iterator;

        if (inst->state == State::RETIRED) {
            iterator = sq.erase(iterator);
            retired_counter++;
            sq_size--;
        } else {
            ++iterator;
        }
    }
}

// fetch instructions
// update cycle counter
void cycle_stage_6()
{
    for (unsigned long i = 0; i < f; ++i) {

        proc_inst_t* inst = new proc_inst_t;
        bool success = read_instruction(inst);

        if (success) {

            // initialize instruction
            inst->fu = abs(inst->op_code);
            inst->inst_tag = inst_tag_counter++;
            inst->dest_tag = UINT32_MAX;
            inst->src_ready[0] = false;
            inst->src_ready[1] = false;
            inst->state = State::FETCHED;

            char log_line[80];
            sprintf(log_line, "%lu\tFETCHED\t%u\n", cycle_counter, inst->inst_tag);
            log_file << log_line;

            inst->fetch = cycle_counter;
            inst->disp = cycle_counter + 1;

            instructions.push_back(inst);

            // insert instruction into dispatch queue
            dq.push_back(inst);
            dq_size++;

            // update max size
            if (dq_size > dq_max_size) {
                dq_max_size = dq_size;
            }
        }
    }

    dq_size_sum += dq_size;
    cycle_counter++;
}

//================//
// Driver Methods //
//================//

/**
 * Subroutine for initializing the processor. You many add and initialize any global or heap
 * variables as needed.
 * XXX: You're responsible for completing this routine
 *
 * @r Number of result buses
 * @k0 Number of k0 FUs
 * @k1 Number of k1 FUs
 * @k2 Number of k2 FUs
 * @f Number of instructions to fetch
 */
void setup_proc(uint64_t r, uint64_t k0, uint64_t k1, uint64_t k2, uint64_t f) 
{
    log_file.open("log");

    ::r = r;
    k[0] = k0;
    k[1] = k1;
    k[2] = k2;
    ::f = f;

    sq_max_size = 2 * (k0 + k1 + k2);

    reg = new reg_t[128];
    cdb = new proc_inst_t*[r];

    for (int i = 0; i < 128; ++i) {
        reg[i].tag = i;
    }
}

/**
 * Subroutine that simulates the processor.
 *   The processor should fetch instructions as appropriate, until all instructions have executed
 * XXX: You're responsible for completing this routine
 *
 * @p_stats Pointer to the statistics structure
 */
void run_proc(proc_stats_t* p_stats)
{
    log_file << "CYCLE\tOPERATION\tINSTRUCTION\n";

    do {

        cycle_stage_0();
        cycle_stage_1();
        cycle_stage_2();
        cycle_stage_3();
        cycle_stage_4();
        cycle_stage_5();
        cycle_stage_6();

    } while (inst_tag_counter != retired_counter);
}

/**
 * Subroutine for cleaning up any outstanding instructions and calculating overall statistics
 * such as average IPC, average fire rate etc.
 * XXX: You're responsible for completing this routine
 *
 * @p_stats Pointer to the statistics structure
 */
void complete_proc(proc_stats_t *p_stats) 
{
    log_file.close();

    cycle_counter--;
    inst_tag_counter--;
    retired_counter--;

    p_stats->cycle_count = cycle_counter;
    p_stats->retired_instruction = inst_tag_counter;
    p_stats->max_disp_size = dq_max_size;
    p_stats->avg_disp_size = (float) (((double) dq_size_sum) / ((double) cycle_counter));
    p_stats->avg_inst_fired = (float) (((double) retired_counter) / ((double) cycle_counter));
    p_stats->avg_inst_retired = p_stats->avg_inst_fired;

    print_instructions();
    printf("\n");

    delete[] reg;
    delete[] cdb;  
}

void print_instructions()
{
    printf("INST\tFETCH\tDISP\tSCHED\tEXEC\tSTATE\n");

    for (list<proc_inst_t*>::iterator iterator = instructions.begin(); iterator != instructions.end(); ++iterator) {

        proc_inst_t* inst = *iterator;
        printf("%d\t%d\t%d\t%d\t%d\t%d\n", inst->inst_tag, inst->fetch, inst->disp, inst->sched, inst->exec, inst->update);
        delete inst;
    }
}
