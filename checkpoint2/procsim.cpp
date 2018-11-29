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

// register file and its backups
reg_t* reg;
reg_t* backup_1;
reg_t* backup_2;

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

// reorder buffer
list<proc_inst_t*> rob;

// processor parameters
uint64_t r;
uint64_t k[3];
uint64_t f;
uint64_t e;
uint64_t s;

// counters
unsigned long inst_tag_counter = 1;
unsigned long reg_tag_counter = 128;
unsigned long cycle_counter = 1;
unsigned long fired_counter = 1;
unsigned long retired_counter = 1;
unsigned long flushed_counter = 1;
unsigned long exception_counter = 1;
unsigned long backup_counter = 1;
unsigned long rob_hit_counter = 1;
unsigned long reg_hit_counter = 1;
unsigned long fu_busy_counter[3] = {0, 0, 0};

// trailing pointer for re-fetches
unsigned long trailing_inst_tag = 1;
list<proc_inst_t*>::iterator trailing_ptr = instructions.begin();

// instruction barrier
proc_inst_t* ib1 = nullptr;
proc_inst_t* ib2 = nullptr;

// dummy instruction
proc_inst_t* dummy_inst;

// function pointers
const FP stage_0[3] = {&cycle_stage_0, &cycle_stage_0_rob, &cycle_stage_0_cpr};
const FP stage_1[3] = {&cycle_stage_1, &cycle_stage_1_rob, &cycle_stage_1_cpr};
const FP stage_2[3] = {&cycle_stage_2, &cycle_stage_2_rob, &cycle_stage_2_cpr};
const FP stage_3[3] = {&cycle_stage_3, &cycle_stage_3_rob, &cycle_stage_3_cpr};
const FP stage_4[3] = {&cycle_stage_4, &cycle_stage_4_rob, &cycle_stage_4_cpr};
const FP stage_5[3] = {&cycle_stage_5, &cycle_stage_5_rob, &cycle_stage_5_cpr};
const FP stage_6[3] = {&cycle_stage_6, &cycle_stage_6_rob, &cycle_stage_6_cpr};

//====================//
//====================//
//      TOMASULO      //
//====================//
//====================//

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
        cdb[i] = dummy_inst;
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
            fired_counter++;
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
                reg_hit_counter++;

            } else {

                inst->src_tag[i] = reg[inst->src_reg[i]].tag;
                inst->src_ready[i] = false;
                reg_hit_counter++;
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

//=====================//
//=====================//
//   RE-ORDER BUFFER   //
//=====================//
//=====================//

// mark completed intstructions as retired
void cycle_stage_0_rob()
{
    unsigned long retired = 0;

    for (list<proc_inst_t*>::iterator iterator = rob.begin(); iterator != rob.end(); ++iterator) {

        proc_inst_t* inst = *iterator;

        if (inst->state == State::COMPLETED) {

            if (inst->exception) {

                inst->exception = false;

                char log_line[80];
                sprintf(log_line, "%lu\tEXCEPTION\t%u\n", cycle_counter, inst->inst_tag);
                log_file << log_line;

                // handle exception
                exception_counter++;
                flushed_counter += (sq_size - retired);

                rob.clear();
                dq.clear();
                sq.clear();
                sb.clear();

                dq_size = 0;
                sq_size = 0;

                for (int i = 0; i < 3; ++i) {
                    fu_busy_counter[i] = 0;
                }

                for (unsigned long i = 0; i < r; ++i) {
                    cdb[i] = dummy_inst;
                }

                for (int i = 0; i < 128; ++i) {
                    reg[i].tag = reg_tag_counter++;
                    reg[i].ready = true;
                }

                trailing_inst_tag = inst->inst_tag;
                unsigned long i = 1;
                for (list<proc_inst_t*>::iterator iterator = instructions.begin(); iterator != instructions.end(); ++iterator) {
                    if (i == trailing_inst_tag) {
                        trailing_ptr = iterator;
                        break;
                    }
                    i++;
                }

                cycle_counter++;
                break;

            } else {

                inst->state = State::RETIRED;
                retired_counter++;
                retired++;

                char log_line[80];
                sprintf(log_line, "%lu\tSTATE UPDATE\t%u\n", cycle_counter, inst->inst_tag);
                log_file << log_line;

                inst->update = cycle_counter;
            }

        } else {

            break;
        }
    }
}

// broadcast results on result buses
// mark instructions as completed
// update register files
void cycle_stage_1_rob()
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

            char log_line[80];
            sprintf(log_line, "%lu\tBROADCASTED\t%u\n", cycle_counter, inst->inst_tag);
            log_file << log_line;

            iterator = sb.erase(iterator);

        } else if (inst->state == State::FIRED) {

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
        cdb[i] = dummy_inst;
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
void cycle_stage_2_rob()
{
    for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end(); ++iterator) {

        proc_inst_t* inst = *iterator;

        if (inst->src_ready[0]
            && inst->src_ready[1]
            && fu_busy_counter[inst->fu] < k[inst->fu]
            && inst->state == State::DISPATCHED) {

            inst->state = State::FIRED;
            inst->fired_cycle = cycle_counter;
            fired_counter++;
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
void cycle_stage_3_rob()
{
    // for each result bus
    for (unsigned long j = 0; j < r; ++j) {
        // for each instruction
        for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end(); ++iterator) {
            // for each source register
            for (unsigned long k = 0; k < 2; ++k) {

                proc_inst_t* inst = *iterator;

                if (inst->state == State::DISPATCHED
                    && cdb[j]->dest_tag == inst->src_tag[k]) {

                    inst->src_ready[k] = true;
                }
            }
        }
    }
}

// dispatch instructions to scheduling queue
// scheduling queue reads register file
void cycle_stage_4_rob()
{
    // update max size
    if (dq_size > dq_max_size) {
        dq_max_size = dq_size;
    }

    dq_size_sum += dq_size;

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

            if (inst->src_reg[i] != -1) {

                // check ROB
                bool found = false;
                for (list<proc_inst_t*>::reverse_iterator iterator = rob.rbegin(); iterator != rob.rend(); ++iterator) {

                    proc_inst_t* rob_inst = *iterator;

                    if (rob_inst->dest_reg == inst->src_reg[i]) {

                        found = true;
                        break;
                    }
                }

                // check register file
                if (found) {
                    rob_hit_counter++;
                } else {
                    reg_hit_counter++;
                }
            }
        }

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

        rob.push_back(inst);
        rob.sort([](proc_inst_t* x, proc_inst_t* y) {
            return x->inst_tag < y->inst_tag;
        });
    }

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
void cycle_stage_5_rob()
{
    for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end();) {

        proc_inst_t* inst = *iterator;

        if (inst->state == State::RETIRED) {
            iterator = sq.erase(iterator);
            sq_size--;
        } else {
            ++iterator;
        }
    }

    for (list<proc_inst_t*>::iterator iterator = rob.begin(); iterator != rob.end();) {

        proc_inst_t* inst = *iterator;

        if (inst->state == State::RETIRED) {
            iterator = rob.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

// fetch instructions
// update cycle counter
void cycle_stage_6_rob()
{
    for (unsigned long i = 0; i < f; ++i) {

        bool trailing = trailing_inst_tag < inst_tag_counter;
        proc_inst_t* inst;
        bool success;

        if (trailing) {

            inst = *trailing_ptr;
            success = true;

        } else {

            inst = new proc_inst_t;
            success = read_instruction(inst);
        }

        if (success) {

            // initialize instruction
            inst->dest_tag = UINT32_MAX;
            inst->src_ready[0] = false;
            inst->src_ready[1] = false;
            inst->state = State::FETCHED;

            if (trailing) {

                char log_line[80];
                sprintf(log_line, "%lu\tRE-FETCHED\t%u\n", cycle_counter, inst->inst_tag);
                log_file << log_line;

            } else {

                inst->fu = abs(inst->op_code);
                inst->exception = !(inst_tag_counter % e);
                inst->inst_tag = inst_tag_counter++;
                char log_line[80];
                sprintf(log_line, "%lu\tFETCHED\t%u\n", cycle_counter, inst->inst_tag);
                log_file << log_line;
            }

            trailing_inst_tag++;

            inst->fetch = cycle_counter;
            inst->disp = cycle_counter + 1;

            if (instructions.empty()) {
                instructions.push_back(inst);
                trailing_ptr = instructions.begin();
            } else if (!trailing) {
                instructions.push_back(inst);
                trailing_ptr++;
            } else {
                trailing_ptr++;
            }

            // insert instruction into dispatch queue
            dq.push_back(inst);
            dq_size++;

            // update max size
            //if (dq_size > dq_max_size) {
            //    dq_max_size = dq_size;
            //}
        }
    }

    //dq_size_sum += dq_size;
    cycle_counter++;
}

//=====================//
//=====================//
//  CHECKPOINT REPAIR  //
//=====================//
//=====================//

// mark completed intstructions as retired
void cycle_stage_0_cpr()
{
    for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end(); ++iterator) {

        proc_inst_t* inst = *iterator;

        if (inst->state == State::COMPLETED) {

            if (inst->exception) {

                inst->exception = false;

                char log_line[80];
                sprintf(log_line, "%lu\tEXCEPTION\t%u\n", cycle_counter, inst->inst_tag);
                log_file << log_line;

                // handle exception
                exception_counter++;
                flushed_counter += sq.back()->inst_tag - ib2->inst_tag;

                dq.clear();
                sq.clear();
                sb.clear();

                dq_size = 0;
                sq_size = 0;

                for (int i = 0; i < 3; ++i) {
                    fu_busy_counter[i] = 0;
                }

                for (unsigned long i = 0; i < r; ++i) {
                    cdb[i] = dummy_inst;
                }

                for (int i = 0; i < 128; ++i) {
                    reg[i].tag = backup_2[i].tag;
                    backup_1[i].tag = backup_2[i].tag;
                    reg[i].ready = true;
                }

                trailing_inst_tag = ib2->inst_tag + 1;
                unsigned long i = 1;
                for (list<proc_inst_t*>::iterator iterator = instructions.begin(); iterator != instructions.end(); ++iterator) {
                    if (i == trailing_inst_tag) {
                        trailing_ptr = iterator;
                        break;
                    }
                    i++;
                }

                cycle_counter++;
                break;

            } else {

                inst->state = State::RETIRED;

                char log_line[80];
                sprintf(log_line, "%lu\tSTATE UPDATE\t%u\n", cycle_counter, inst->inst_tag);
                log_file << log_line;

                inst->update = cycle_counter;
            }

            int count = 0;

            for (list<proc_inst_t*>::iterator _iterator = sq.begin(); _iterator != sq.end(); ++_iterator) {

                proc_inst_t* _inst = *_iterator;
                uint32_t ib_inst_tag = (ib1 == nullptr ? 20 : ib1->inst_tag);

                if (_inst->inst_tag <= ib_inst_tag &&
                    _inst->state != State::RETIRED) {

                    count++;
                }
            }

            if (count == 0) {

                char log_line[80];
                sprintf(log_line, "%lu\tBACKUP2\t%u\tTO\t%u\n", cycle_counter, ib2 == nullptr ? 1 : ib2->inst_tag + 1, ib1->inst_tag);
                log_file << log_line;

                for (int i = 0; i < 128; ++i) {
                    backup_2[i] = backup_1[i];
                    backup_1[i] = reg[i];
                }

                retired_counter += (ib2 == nullptr ? 20 : ib1->inst_tag - ib2->inst_tag);
                backup_counter++;

                ib2 = ib1;
                ib1 = sq.back();
            }
        }
    }
}

// broadcast results on result buses
// mark instructions as completed
// update register files
void cycle_stage_1_cpr()
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

            char log_line[80];
            sprintf(log_line, "%lu\tBROADCASTED\t%u\n", cycle_counter, inst->inst_tag);
            log_file << log_line;

            iterator = sb.erase(iterator);

        } else if (inst->state == State::FIRED) {

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
        cdb[i] = dummy_inst;
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
void cycle_stage_2_cpr()
{
    for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end(); ++iterator) {

        proc_inst_t* inst = *iterator;

        if (inst->src_ready[0]
            && inst->src_ready[1]
            && fu_busy_counter[inst->fu] < k[inst->fu]
            && inst->state == State::DISPATCHED) {

            inst->state = State::FIRED;
            inst->fired_cycle = cycle_counter;
            fired_counter++;
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
void cycle_stage_3_cpr()
{
    // for each result bus
    for (unsigned long j = 0; j < r; ++j) {
        // for each instruction
        for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end(); ++iterator) {
            // for each source register
            for (unsigned long k = 0; k < 2; ++k) {

                proc_inst_t* inst = *iterator;

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
void cycle_stage_4_cpr()
{
    // update max size
    if (dq_size > dq_max_size) {
        dq_max_size = dq_size;
    }

    dq_size_sum += dq_size;

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
                reg_hit_counter++;

            } else {

                inst->src_tag[i] = reg[inst->src_reg[i]].tag;
                inst->src_ready[i] = false;
                reg_hit_counter++;
            }
        }

        // assign new tag to destination register
        if (inst->dest_reg > -1) {

            reg[inst->dest_reg].tag = reg_tag_counter;
            reg[inst->dest_reg].ready = false;
            uint32_t ib_inst_tag = (ib1 == nullptr ? 20 : ib1->inst_tag);
            if (inst->inst_tag <= ib_inst_tag) {
                backup_1[inst->dest_reg].tag = reg_tag_counter;
                backup_1[inst->dest_reg].ready = false;
            }
            inst->dest_tag = reg_tag_counter;
            reg_tag_counter++;
        }

        sq.push_back(inst);
        sq_size++;
    }

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
void cycle_stage_5_cpr()
{
    for (list<proc_inst_t*>::iterator iterator = sq.begin(); iterator != sq.end();) {

        proc_inst_t* inst = *iterator;

        if (inst->state == State::RETIRED) {
            iterator = sq.erase(iterator);
            sq_size--;
        } else {
            ++iterator;
        }
    }
}

// fetch instructions
// update cycle counter
void cycle_stage_6_cpr()
{
    for (unsigned long i = 0; i < f; ++i) {

        bool trailing = trailing_inst_tag < inst_tag_counter;
        proc_inst_t* inst;
        bool success;

        if (trailing) {

            inst = *trailing_ptr;
            success = true;

        } else {

            inst = new proc_inst_t;
            success = read_instruction(inst);
        }

        if (success) {

            // initialize instruction
            inst->dest_tag = UINT32_MAX;
            inst->src_ready[0] = false;
            inst->src_ready[1] = false;
            inst->state = State::FETCHED;

            if (trailing) {

                char log_line[80];
                sprintf(log_line, "%lu\tRE-FETCHED\t%u\n", cycle_counter, inst->inst_tag);
                log_file << log_line;

            } else {

                inst->fu = abs(inst->op_code);
                inst->exception = !(inst_tag_counter % e);
                inst->inst_tag = inst_tag_counter++;
                char log_line[80];
                sprintf(log_line, "%lu\tFETCHED\t%u\n", cycle_counter, inst->inst_tag);
                log_file << log_line;
            }

            trailing_inst_tag++;

            inst->fetch = cycle_counter;
            inst->disp = cycle_counter + 1;

            if (instructions.empty()) {
                instructions.push_back(inst);
                trailing_ptr = instructions.begin();
            } else if (!trailing) {
                instructions.push_back(inst);
                trailing_ptr++;
            } else {
                trailing_ptr++;
            }

            // first instruction barrier
            if (ib1 == nullptr && inst->inst_tag == 20) {
                ib1 = inst;
            }

            // insert instruction into dispatch queue
            dq.push_back(inst);
            dq_size++;

            // update max size
            //if (dq_size > dq_max_size) {
            //    dq_max_size = dq_size;
            //}
        }
    }

    //dq_size_sum += dq_size;
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
void setup_proc(uint64_t r, uint64_t k0, uint64_t k1, uint64_t k2, uint64_t f, uint64_t e, uint64_t s) 
{
    log_file.open("log");

    ::r = r;
    k[0] = k0;
    k[1] = k1;
    k[2] = k2;
    ::f = f;
    ::e = e;
    ::s = s;

    if (e == 0) {
        ::e = UINT64_MAX;
    }

    sq_max_size = 2 * (k0 + k1 + k2);

    reg = new reg_t[128];
    backup_1 = new reg_t[128];
    backup_2 = new reg_t[128];

    cdb = new proc_inst_t*[r];

    dummy_inst = new proc_inst_t;
    dummy_inst->inst_tag = UINT32_MAX;
    dummy_inst->dest_tag = UINT32_MAX;

    for (int i = 0; i < 128; ++i) {
        reg[i].tag = i;
        backup_1[i].tag = i;
        backup_2[i].tag = i;
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
    bool debug = false;

    do {

        if (debug) printf("begin stage 0\n");
        stage_0[s]();
        if (debug) printf("begin stage 1\n");
        stage_1[s]();
        if (debug) printf("begin stage 2\n");
        stage_2[s]();
        if (debug) printf("begin stage 3\n");
        stage_3[s]();
        if (debug) printf("begin stage 4\n");
        stage_4[s]();
        if (debug) printf("begin stage 5\n");
        stage_5[s]();
        if (debug) printf("begin stage 6\n");
        stage_6[s]();

    } while (!(sq.empty() && dq.empty()));
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
    fired_counter--;
    retired_counter--;
    flushed_counter--;
    exception_counter--;
    backup_counter--;
    rob_hit_counter--;
    reg_hit_counter--;

    p_stats->cycle_count = cycle_counter;
    p_stats->retired_instruction = retired_counter;
    p_stats->max_disp_size = dq_max_size;
    p_stats->avg_disp_size = (float) (((double) dq_size_sum) / ((double) cycle_counter));
    p_stats->avg_inst_fired = (float) (((double) fired_counter) / ((double) cycle_counter));
    p_stats->avg_inst_retired = (float) (((double) retired_counter) / ((double) cycle_counter));
    p_stats->reg_file_hit_count = reg_hit_counter;
    p_stats->rob_hit_count = rob_hit_counter;
    p_stats->exception_count = exception_counter;
    p_stats->backup_count = backup_counter;
    p_stats->flushed_count = flushed_counter;

    print_instructions();

    delete[] reg;
    delete[] backup_1;
    delete[] backup_2;
    delete[] cdb;
    delete dummy_inst;
}

void print_instructions()
{
    printf("INST\tFETCH\tDISP\tSCHED\tEXEC\tSTATE\n");

    for (list<proc_inst_t*>::iterator iterator = instructions.begin(); iterator != instructions.end(); ++iterator) {

        proc_inst_t* inst = *iterator;
        printf("%d\t%d\t%d\t%d\t%d\t%d\n", inst->inst_tag, inst->fetch, inst->disp, inst->sched, inst->exec, inst->update);
        delete inst;
    }

    printf("\n");
}
