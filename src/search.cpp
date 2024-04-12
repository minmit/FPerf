//
//  search.cpp
//  FPerf
//
//  Created by Mina Tahmasbi Arashloo on 11/20/20.
//  Copyright © 2020 Mina Tahmasbi Arashloo. All rights reserved.
//

#include <cmath>

#include "search.hpp"
#include "global_vars.h"

#include <filesystem>

unsigned long MAX_COST;

Search::Search(ContentionPoint* cp,
               Query query,
               unsigned int max_spec,
               SharedConfig* shared_config,
               string good_examples_fname,
               string bad_examples_fname):
cp(cp),
query(query),
max_spec(max_spec),
shared_config(shared_config),
dists(shared_config->get_dists()),
spec_factory(SpecFactory(shared_config)),
wl_last_step(
    Workload(max_spec, shared_config->get_in_queue_cnt(), shared_config->get_total_time())) {
    target_queues = shared_config->get_target_queues();
    in_queue_cnt = shared_config->get_in_queue_cnt();
    total_time = shared_config->get_total_time();

    input_only_solver = new InputOnlySolver();

    // Process bad examples
    deque<Example*> bad_full_examples;
    read_examples_from_file(bad_full_examples, bad_examples_fname);
    for (deque<Example*>::iterator it = bad_full_examples.begin(); it != bad_full_examples.end();
         it++) {
        bad_examples.push_back(cp->index_example(*it));
    }

    // Process good examples
    deque<Example*> good_full_examples;
    read_examples_from_file(good_full_examples, good_examples_fname);
    for (deque<Example*>::iterator it = good_full_examples.begin(); it != good_full_examples.end();
         it++) {
        good_examples.push_back(cp->index_example(*it));
        cout << *(cp->index_example(*it)) << endl;
    }

    MAX_COST = ((bad_examples.size() * BAD_EXAMPLE_WEIGHT +
                 good_examples.size() * GOOD_EXAMPLE_WEIGHT) *
                    EXAMPLE_WEIGHT_IN_COST +
                1);
    // MAX_COST = 4;
}

Search::Search(ContentionPoint* cp,
               Query query,
               unsigned int max_spec,
               SharedConfig* shared_config,
               deque<IndexedExample*>& good_ex,
               deque<IndexedExample*>& bad_ex):
cp(cp),
query(query),
max_spec(max_spec),
shared_config(shared_config),
dists(shared_config->get_dists()),
spec_factory(SpecFactory(shared_config)),
wl_last_step(
    Workload(max_spec, shared_config->get_in_queue_cnt(), shared_config->get_total_time())) {
    target_queues = shared_config->get_target_queues();
    in_queue_cnt = shared_config->get_in_queue_cnt();
    total_time = shared_config->get_total_time();

    input_only_solver = new InputOnlySolver();

    // Process bad examples
    for (deque<IndexedExample*>::iterator it = bad_ex.begin(); it != bad_ex.end(); it++) {
        bad_examples.push_back(*it);
    }

    // Process good examples
    for (deque<IndexedExample*>::iterator it = good_ex.begin(); it != good_ex.end(); it++) {
        good_examples.push_back(*it);
    }

    MAX_COST = ((bad_examples.size() * BAD_EXAMPLE_WEIGHT +
                 good_examples.size() * GOOD_EXAMPLE_WEIGHT) *
                    EXAMPLE_WEIGHT_IN_COST +
                1);
    // MAX_COST = 4;
}


void Search::init_wl(Workload& wl) {
    wl.clear();
}

bool Search::satisfies_bad_example(Workload wl) {
    for (deque<IndexedExample*>::iterator it = bad_examples.begin(); it != bad_examples.end();
         it++) {
        call_cnt++;

        //        auto start = noww();
        bool satisfies = cp->workload_satisfies_example(wl, *it);
        //        DEBUG_MSG("call time: " << get_diff_microsec(start, noww()) << endl);

        if (satisfies) {
            //            DEBUG_MSG("bad example" << endl << **it << endl);
            return true;
        }
    }
    return false;
}

bool Search::check(Workload wl, string function_name) {
    DEBUG_MSG("checking: " << endl << wl << endl);
    time_typ start_time = noww();

    last_input_infeasible = false;

    // check if it matches one of the bad examples
    // Removed for benchmarking purposes
//    if (satisfies_bad_example(wl)) {
//        DEBUG_MSG("matched bad example" << endl);
//        if(no_solver_call.find(function_name) == no_solver_call.end())
//            no_solver_call[function_name] = 1;
//        else
//            no_solver_call[function_name]++;
//        return false;
//    }

    // TODO: Make use of a real input_only_solver
    solver_res_t input_only_res = input_only_solver->check_workload_without_query(wl);

    bool res = false;

    IndexedExample* counter_eg = new IndexedExample();
    bool used_example = false;

    switch (input_only_res) {
        case solver_res_t::SAT: {
            if(input_only_solver_call.find(function_name) == input_only_solver_call.end())
                input_only_solver_call[function_name] = 1;
            else
                input_only_solver_call[function_name]++;
            if(query_only_solver_call.find(function_name) == query_only_solver_call.end())
                query_only_solver_call[function_name] = 1;
            else
                query_only_solver_call[function_name]++;

            // wl & !query
            solver_res_t query_only_res = cp->check_workload_with_query(wl, counter_eg);

            // add counter example
            if (query_only_res == solver_res_t::SAT) {
                bad_examples.push_back(counter_eg);
                used_example = true;
            }
            if (query_only_res == solver_res_t::UNKNOWN) {
                cout << "UNKNOWN from full solver::check_without_query for workload: " << endl;
                cout << wl << endl;
            }

            res = query_only_res == solver_res_t::UNSAT;
            break;
        }
        case solver_res_t::UNSAT: {
            if(input_only_solver_call.find(function_name) == input_only_solver_call.end())
                input_only_solver_call[function_name] = 1;
            else
                input_only_solver_call[function_name]++;
            infeasible_input_cnt++;
            last_input_infeasible = true;
            res = false;

            break;
        }
        case solver_res_t::UNKNOWN: {
            if(full_solver_call.find(function_name) == full_solver_call.end())
                full_solver_call[function_name] = 1;
            else
                full_solver_call[function_name]++;

            solver_res_t input_feasible = cp->check_workload_without_query(wl);

            DEBUG_MSG("input feasible: " << input_feasible << endl);

            switch (input_feasible) {
                case solver_res_t::SAT: {
                    solver_res_t query_only_res = cp->check_workload_with_query(wl, counter_eg);

                    DEBUG_MSG("not query satisfied " << query_only_res << endl);

                    // add counter example
                    if (query_only_res == solver_res_t::SAT) {
                        bad_examples.push_back(counter_eg);
                        used_example = true;
                        DEBUG_MSG(*counter_eg << endl);
                    }
                    if (query_only_res == solver_res_t::UNKNOWN) {
                        cout << "UNKNOWN from full solver::check_without_query for workload: "
                             << endl;
                        cout << wl << endl;
                    }

                    res = query_only_res == solver_res_t::UNSAT;
                    break;
                }
                case solver_res_t::UNSAT: {
                    infeasible_input_cnt++;
                    last_input_infeasible = true;
                    res = false;
                    break;
                }
                case solver_res_t::UNKNOWN: {
                    res = false;
                    cout << "UNKNOWN from full solver::check_without_query for workload: " << endl;
                    cout << wl << endl;
                    break;
                }
            }
            break;
        }
    }

    // TODO: It only being populated in SAT case, where we need the eg.
    //  Otherwise it does not get populated, thus no need for deletion.
    if (!used_example) {
        delete counter_eg;
    }

    //------------ Timing Stats
    time_typ end_time = noww();
//    unsigned long long int seconds = get_diff_sec(start_time, end_time);
    unsigned long long int ms = get_diff_millisec(start_time, end_time);
    if(sum_check_time.find(function_name) == sum_check_time.end())
        sum_check_time[function_name] = ms;
    else
        sum_check_time[function_name] += ms;
    if(max_check_time.find(function_name) == max_check_time.end())
        max_check_time[function_name] = ms;
    else if (ms > max_check_time[function_name]) max_check_time[function_name] = ms;
    //-------------------------

    return res;
}

void Search::search(Workload wl) {
    init_wl(wl);
    close_count = 0;
    infeasible_wls.clear();

    bool found = false;
    unsigned int solution_refinement_rounds = 0;

    unsigned int backtrack_count = 0;
    bool local_search = false;

    unsigned int cost_now = cost(wl);

    round_no = 1;

    while (!found) {

        /* ******************** check the current workload ******************** */

        // print current wl
        cout << "----------------------------------------------------------" << endl;
        if (call_cnt > 0) {
//            DEBUG_MSG("avg call time: " << (sum_call_time / call_cnt) << endl);
        }
        cout << "round " << round_no << endl;
        cout << wl << endl;

        // check if it is the answer


        Workload to_check(max_spec + in_queue_cnt, in_queue_cnt, total_time);
        to_check = wl;
        for (unsigned int q = 0; q < cp->in_queue_cnt(); q++) {
            if (target_queues.find(q) == target_queues.end()) {
                Indiv n_indiv = Indiv(metric_t::CENQ, q);
                Comp n_wl_spec = Comp(n_indiv, op_t::LE, 0u);
                TimedSpec n_spec = TimedSpec(n_wl_spec, total_time, total_time);
                to_check.add_spec(n_spec);
            }
        }
        // true <=> (wl & !query): UNSAT
        // false -> bad_examples += eg
        bool satisfies_query = check(to_check);
        // bool satisfies_query = check(wl);

        // exit the loop if we have found the answer
        if (satisfies_query) {
            DEBUG_MSG("FOUND!!!!" << endl);
            solutions.push_back(wl);
            found = true;
        }
        if (found) {
            // TODO: Currently the SOLUTION_REFINEMENT_MAX_ROUNDS is zero
            solution_refinement_rounds++;
            if (solution_refinement_rounds > SOLUTION_REFINEMENT_MAX_ROUNDS) break;
        }


        // Undo the last move if the current spec if infeasible
        // record the infeasible wl
        // last_input_infeasible <=> (base_wl & wl): UNSAT
        if (last_input_infeasible) {
            infeasible_wls.push_back(wl);
            wl = wl_last_step;
            cout << "Undoing last step " << endl;
            cout << "----------------------------------------------------------" << endl;
            cout << wl << endl;
            backtrack_count++;
        } else {
            infeasible_wls.clear();
            backtrack_count = 0;
        }

        /* ******************** decide the next workload ******************** */
        // if not making progress, switch to local search or reset
        if (close_count > LOCAL_SEARCH_THRESH)
            local_search = true;
        else
            local_search = false;

        if (close_count > RESET_THRESH_SLOW_PROGRESS || backtrack_count > RESET_THRESH_BACKTRACK) {

            cout << banner("RESETTING") << endl;
            reset_cnt++;
            init_wl(wl);
            close_count = 0;
            backtrack_count = 0;
            local_search = false;
            infeasible_wls.clear();
        }

        cost_now = cost(wl);
        wl_last_step = wl;

        bool found_next = false;

        // TODO: if there are many undos, maybe taking the min is not good?
        //  we might be biased towards infeasible inputs

        DEBUG_MSG("local search: " << local_search << endl);

        // Find a modified workload with lower cost
        while (!found_next) {
            unsigned int hops = 1;
            if (local_search) hops = LOCAL_SEARCH_MAX_HOPS;


            DEBUG_MSG("hops: " << hops << endl);

            Workload candidate = random_neighbor(wl, hops);

            unsigned int candidate_cost = cost(candidate);

            if (candidate_cost < cost_now) {
                found_next = true;
                wl = candidate;
            } else {
                double scaled_diff = ((double) (candidate_cost - cost_now + 1)) /
                                     EXAMPLE_WEIGHT_IN_COST;
                double cutoff = exp(scaled_diff * -COST_LAMBDA);
                double rand = dists->real_zero_to_one();

                DEBUG_MSG("greater jump: " << scaled_diff << " " << cutoff << " " << rand << endl);

                if (rand <= cutoff) {
                    found_next = true;
                    wl = candidate;
                }
            }

            if (found_next) {
                DEBUG_MSG("candidate: "
                          << candidate_cost << ": " << good_example_match_count(candidate) << " "
                          << bad_example_match_count(candidate) << " " << candidate.cost() << endl);
                DEBUG_MSG(candidate << endl);
            }
        }

        unsigned int candidate_cost = cost(wl);

        // See if you are making progress
        // TODO: should we do the whole cost?
        unsigned int progress = (cost_now > candidate_cost) ? (cost_now - candidate_cost)
                                                            : (candidate_cost - cost_now);

        double progress_thresh = NEGLIGIBLE_PROGRESS_FRAC * MAX_COST;
        if (progress <= progress_thresh)
            close_count++;
        else
            close_count = 0;

        DEBUG_MSG("close count: " << close_count << " " << progress_thresh << endl);

        cost_now = cost(wl);

        if (round_no % 10 == 0) {
            print_stats();
        }
        round_no++;

        if (local_search) rounds_in_local_search++;
    }
}

// TODO: Most of the refinement steps are only applied on COMP
Workload Search::refine(Workload wl) {

    // Add specs for "zero queues" back in
    for (unsigned int q = 0; q < cp->in_queue_cnt(); q++) {
        if (target_queues.find(q) == target_queues.end()) {
            Indiv n_indiv = Indiv(metric_t::CENQ, q);
            Comp n_wl_spec = Comp(n_indiv, op_t::LE, 0u);
            wl.add_spec(TimedSpec(n_wl_spec, total_time, total_time));
        }
    }

    Workload candidate(max_spec, in_queue_cnt, total_time);

    // Try removing each spec to see if
    // we can make the workload more general
    bool done = false;

    time_typ start = noww();
    while (!done) {

        bool changed = false;

        set<TimedSpec> specs = wl.get_all_specs();
        for (set<TimedSpec>::iterator it = specs.begin(); it != specs.end(); it++) {
            candidate = wl;
            candidate.rm_spec(*it);

            if (candidate.is_empty() || candidate.is_all()) continue;

            bool is_ans = check(candidate);
            if (is_ans) {
                wl = candidate;
                changed = true;
                break;
            }
        }

        done = !changed;
    }

    cout << "refinement 1 time: " << (get_diff_millisec(start, noww()) / 1000.0) << " s" << endl;

    start = noww();

    // Change specs related to per-packet
    // meta data to CENQ to broaden the workload
    set<TimedSpec> specs = wl.get_all_specs();
    for (set<TimedSpec>::iterator it = specs.begin(); it != specs.end(); it++) {
        TimedSpec tspec = *it;
        wl_spec_t wspec = tspec.get_wl_spec();
        // TODO: Generalize to other constructs?
        if (!holds_alternative<Comp>(wspec)) continue;
        Comp comp = get<Comp>(wspec);
        lhs_t lhs = comp.get_lhs();
        if (holds_alternative<Indiv>(lhs)) {
            Indiv indiv = get<Indiv>(lhs);
            metric_t m = indiv.get_metric();
            // TODO: This assumes that rhs is constant
            if (m == metric_t::DST || m == metric_t::ECMP) {
                unsigned int q = indiv.get_queue();
                unsigned int c = get<unsigned int>(comp.get_rhs());
                if (c < 1) c = 1;

                candidate = wl;
                Indiv n_indiv = Indiv(metric_t::CENQ, q);
                Comp n_comp = Comp(n_indiv, op_t::GE, Time(c));
                TimedSpec new_spec = TimedSpec(n_comp, tspec.get_time_range(), total_time);
                candidate.mod_spec(*it, new_spec);


                if (candidate.is_empty() || candidate.is_all()) continue;

                bool is_ans = check(candidate);
                if (is_ans) {
                    wl = candidate;
                }
            }
        }
    }

    cout << "refinement 3 time: " << (get_diff_millisec(start, noww()) / 1000.0) << " s" << endl;

    start = noww();
    // Find tighter bounds when the
    // right hand side is time or constant
    specs = wl.get_all_specs();
    for (set<TimedSpec>::iterator it = specs.begin(); it != specs.end(); it++) {
        TimedSpec tspec = *it;
        wl_spec_t wspec = tspec.get_wl_spec();
        if (!holds_alternative<Comp>(wspec)) continue;
        Comp comp = get<Comp>(wspec);
        rhs_t rhs = comp.get_rhs();
        op_t op = comp.get_op();
        if (op == op_t::GT || op == op_t::GE) {
            if (holds_alternative<Time>(rhs)) {
                Time time = get<Time>(rhs);
                unsigned int coeff = time.get_coeff();
                Workload last_working_candidate = wl;
                bool coeff_changed = false;

                for (int new_coeff = coeff - 1; new_coeff >= 1; new_coeff--) {

                    candidate = wl;
                    unsigned int n_coeff = (unsigned int) new_coeff;
                    Time n_time = Time(n_coeff);
                    Comp n_comp = Comp(comp.get_lhs(), comp.get_op(), n_time);
                    TimedSpec new_spec = TimedSpec(n_comp, tspec.get_time_range(), total_time);
                    candidate.mod_spec(*it, new_spec);

                    if (candidate.is_empty() || candidate.is_all()) continue;

                    bool is_ans = check(candidate);
                    if (is_ans) {
                        coeff_changed = true;
                        last_working_candidate = candidate;
                    } else {
                        break;
                    }
                }

                if (coeff_changed) {
                    wl = last_working_candidate;
                }
            }

            else if (holds_alternative<unsigned int>(rhs)) {
                unsigned int c = get<unsigned int>(rhs);
                Workload last_working_candidate = wl;
                bool c_changed = false;

                for (int new_c = c - 1; new_c >= 0; new_c--) {

                    candidate = wl;
                    unsigned int n_c = (unsigned int) new_c;
                    Comp n_comp = Comp(comp.get_lhs(), comp.get_op(), n_c);
                    TimedSpec new_spec = TimedSpec(n_comp, tspec.get_time_range(), total_time);
                    candidate.mod_spec(*it, new_spec);

                    if (candidate.is_empty() || candidate.is_all()) continue;

                    bool is_ans = check(candidate);
                    if (is_ans) {
                        c_changed = true;
                        last_working_candidate = candidate;
                    } else {
                        break;
                    }
                }

                if (c_changed) {
                    wl = last_working_candidate;
                }
            }
        }
    }
    cout << "refinement 4 time: " << (get_diff_millisec(start, noww()) / 1000.0) << " s" << endl;

    start = noww();

    qset_t in_wl;
    specs = wl.get_all_specs();
    for (set<TimedSpec>::iterator it = specs.begin(); it != specs.end(); it++) {
        wl_spec_t spec = it->get_wl_spec();
        if (!holds_alternative<Comp>(spec)) continue;
        Comp comp = get<Comp>(spec);
        lhs_t lhs = comp.get_lhs();

        if (holds_alternative<Indiv>(lhs)) {
            in_wl.insert(get<Indiv>(lhs).get_queue());
        } else {
            qset_t tsum_qset = get<QSum>(lhs).get_qset();
            for (qset_t::iterator it2 = tsum_qset.begin(); it2 != tsum_qset.end(); it2++) {
                in_wl.insert(*it2);
            }
        }
    }

    qset_t zero_in_base;
    set<TimedSpec> base_specs = cp->get_base_workload().get_all_specs();
    for (set<TimedSpec>::iterator it = base_specs.begin(); it != base_specs.end(); it++) {
        wl_spec_t spec = it->get_wl_spec();
        if (!holds_alternative<Comp>(spec)) continue;
        Comp comp = get<Comp>(spec);
        lhs_t lhs = comp.get_lhs();
        rhs_t rhs = comp.get_rhs();

        if (it->get_time_range() == time_range_t(0, cp->get_total_time() - 1) &&
            holds_alternative<unsigned int>(rhs) && get<unsigned int>(rhs) == 0 &&
            comp.get_op() == op_t::LE) {
            if (holds_alternative<Indiv>(lhs)) {
                zero_in_base.insert(get<Indiv>(lhs).get_queue());
            }
            if (holds_alternative<QSum>(lhs)) {
                qset_t tsum_qset = get<QSum>(lhs).get_qset();
                for (qset_t::iterator it2 = tsum_qset.begin(); it2 != tsum_qset.end(); it2++) {
                    zero_in_base.insert(*it2);
                }
            }
        }
    }

    DEBUG_MSG("zero_in_base: " << zero_in_base << endl);
    DEBUG_MSG("in_wl: " << in_wl << endl);
    for (unsigned int q = 0; q < cp->in_queue_cnt(); q++) {
        if (in_wl.find(q) != in_wl.end()) continue;
        if (zero_in_base.find(q) != zero_in_base.end()) continue;

        set<TimedSpec> specs = wl.get_all_specs();
        for (set<TimedSpec>::iterator it = specs.begin(); it != specs.end(); it++) {
            candidate = wl;

            wl_spec_t wl_spec = it->get_wl_spec();
            if (!holds_alternative<Comp>(wl_spec)) continue;
            Comp comp = get<Comp>(wl_spec);
            rhs_t rhs = comp.get_rhs();

            if (holds_alternative<unsigned int>(rhs) && get<unsigned int>(rhs) == 0 &&
                comp.get_op() == op_t::LE)
                continue;

            lhs_t lhs = comp.get_lhs();
            lhs_t new_lhs = lhs;

            if (holds_alternative<Indiv>(lhs)) {
                Indiv tone = get<Indiv>(lhs);

                // TODO: aggregatable
                if (tone.get_metric() != metric_t::CENQ) continue;

                qset_t new_qset;
                new_qset.insert(q);
                new_qset.insert(tone.get_queue());
                new_lhs = QSum(new_qset, tone.get_metric());
            } else if (holds_alternative<QSum>(lhs)) {
                QSum tsum = get<QSum>(lhs);

                // TODO: aggregatable
                if (tsum.get_metric() != metric_t::CENQ) continue;
                qset_t new_qset = tsum.get_qset();
                new_qset.insert(q);
                new_lhs = QSum(new_qset, tsum.get_metric());
            }

            Comp n_comp = Comp(new_lhs, comp.get_op(), comp.get_rhs());
            TimedSpec new_spec = TimedSpec(n_comp, it->get_time_range(), total_time);

            candidate.mod_spec(*it, new_spec);

            if (candidate == wl || candidate.is_empty() || candidate.is_all()) continue;

            DEBUG_MSG(candidate << endl);
            bool is_ans = check(candidate);
            if (is_ans) {
                wl = candidate;
            }
        }
    }
    cout << "refinement 2 time: " << (get_diff_millisec(start, noww()) / 1000.0) << " s" << endl;
    return wl;
}

Workload Search::setup_refinement(Workload wl){
    // Add specs for "zero queues" back in
    for (unsigned int q = 0; q < cp->in_queue_cnt(); q++) {
        if (target_queues.find(q) == target_queues.end()) {
            Indiv n_indiv = Indiv(metric_t::CENQ, q);
            Comp n_wl_spec = Comp(n_indiv, op_t::LE, 0u);
            wl.add_spec(TimedSpec(n_wl_spec, total_time, total_time));
        }
    }

    return wl;
}

Workload Search::remove_specs(Workload wl) { // Randomly remove specs and check if it is still a solution
     Workload candidate(max_spec, in_queue_cnt, total_time);

     // Try removing each spec to see if
     // we can make the workload more general
     bool done = false;

     time_typ start = noww();
     while (!done) {

         bool changed = false;

         set<TimedSpec> specs = wl.get_all_specs();
         for (set<TimedSpec>::iterator it = specs.begin(); it != specs.end(); it++) {
             candidate = wl;
             candidate.rm_spec(*it);

             if (candidate.is_empty() || candidate.is_all()) continue;

             bool is_ans = check(candidate);
             if (is_ans) {
//                 throw std::runtime_error("This should never happen");
                 wl = candidate;
                 changed = true;
                 break;
             }
         }

         done = !changed;
     }

     return wl;
}

Workload Search::tighten_constant_bounds(Workload wl){ // Tighten constant bounds on rhs
    Workload candidate(max_spec, in_queue_cnt, total_time);

    // Find tighter bounds when the
    // right hand side is time or constant
    set<TimedSpec> specs = wl.get_all_specs();
    specs = wl.get_all_specs();
    for (set<TimedSpec>::iterator it = specs.begin(); it != specs.end(); it++) {
        TimedSpec tspec = *it;
        wl_spec_t wspec = tspec.get_wl_spec();
        if (!holds_alternative<Comp>(wspec)) continue;
        Comp comp = get<Comp>(wspec);
        rhs_t rhs = comp.get_rhs();
        op_t op = comp.get_op();
        if (op == op_t::GT || op == op_t::GE) {
            if (holds_alternative<Time>(rhs)) {
                Time time = get<Time>(rhs);
                unsigned int coeff = time.get_coeff();
                Workload last_working_candidate = wl;
                bool coeff_changed = false;

                for (int new_coeff = coeff - 1; new_coeff >= 1; new_coeff--) {

                    candidate = wl;
                    unsigned int n_coeff = (unsigned int) new_coeff;
                    Time n_time = Time(n_coeff);
                    Comp n_comp = Comp(comp.get_lhs(), comp.get_op(), n_time);
                    TimedSpec new_spec = TimedSpec(n_comp, tspec.get_time_range(), total_time);
                    candidate.mod_spec(*it, new_spec);

                    if (candidate.is_empty() || candidate.is_all()) continue;

                    bool is_ans = check(candidate);
                    if (is_ans) {
                        coeff_changed = true;
                        last_working_candidate = candidate;
                        opt_count["tighten_constant_bounds"]++;
                    } else {
                        break;
                    }
                }

                if (coeff_changed) {
                    wl = last_working_candidate;
                }
            }

            else if (holds_alternative<unsigned int>(rhs)) {
                unsigned int c = get<unsigned int>(rhs);
                Workload last_working_candidate = wl;
                bool c_changed = false;

                for (int new_c = c - 1; new_c >= 0; new_c--) {

                    candidate = wl;
                    unsigned int n_c = (unsigned int) new_c;
                    Comp n_comp = Comp(comp.get_lhs(), comp.get_op(), n_c);
                    TimedSpec new_spec = TimedSpec(n_comp, tspec.get_time_range(), total_time);
                    candidate.mod_spec(*it, new_spec);

                    if (candidate.is_empty() || candidate.is_all()) continue;

                    bool is_ans = check(candidate, "tighten_constant_bounds");
                    if (is_ans) {
                        c_changed = true;
                        last_working_candidate = candidate;
                        opt_count["tighten_constant_bounds"]++;
                    } else {
                        break;
                    }
                }

                if (c_changed) {
                    wl = last_working_candidate;
                }
            }
        }
    }

    return wl;
}

Workload Search::aggregate_indivs_to_sums(Workload wl){
    before_cost["aggregate_indivs_to_sums"].push_back(cost(wl, "aggregate_indivs_to_sums"));
    Workload candidate(max_spec, in_queue_cnt, total_time);

    qset_t in_wl;
    set<TimedSpec> specs = wl.get_all_specs();
    specs = wl.get_all_specs();
    for (set<TimedSpec>::iterator it = specs.begin(); it != specs.end(); it++) {
        wl_spec_t spec = it->get_wl_spec();
        if (!holds_alternative<Comp>(spec)) continue;
        Comp comp = get<Comp>(spec);
        lhs_t lhs = comp.get_lhs();

        if (holds_alternative<Indiv>(lhs)) {
            in_wl.insert(get<Indiv>(lhs).get_queue());
        } else {
            qset_t tsum_qset = get<QSum>(lhs).get_qset();
            for (qset_t::iterator it2 = tsum_qset.begin(); it2 != tsum_qset.end(); it2++) {
                in_wl.insert(*it2);
            }
        }
    }

    qset_t zero_in_base;
    set<TimedSpec> base_specs = cp->get_base_workload().get_all_specs();
    for (set<TimedSpec>::iterator it = base_specs.begin(); it != base_specs.end(); it++) {
        wl_spec_t spec = it->get_wl_spec();
        if (!holds_alternative<Comp>(spec)) continue;
        Comp comp = get<Comp>(spec);
        lhs_t lhs = comp.get_lhs();
        rhs_t rhs = comp.get_rhs();

        if (it->get_time_range() == time_range_t(0, cp->get_total_time() - 1) &&
            holds_alternative<unsigned int>(rhs) && get<unsigned int>(rhs) == 0 &&
            comp.get_op() == op_t::LE) {
            if (holds_alternative<Indiv>(lhs)) {
                zero_in_base.insert(get<Indiv>(lhs).get_queue());
            }
            if (holds_alternative<QSum>(lhs)) {
                qset_t tsum_qset = get<QSum>(lhs).get_qset();
                for (qset_t::iterator it2 = tsum_qset.begin(); it2 != tsum_qset.end(); it2++) {
                    zero_in_base.insert(*it2);
                }
            }
        }
    }

    DEBUG_MSG("zero_in_base: " << zero_in_base << endl);
    DEBUG_MSG("in_wl: " << in_wl << endl);
    for (unsigned int q = 0; q < cp->in_queue_cnt(); q++) {
        if (in_wl.find(q) != in_wl.end()) continue;
        if (zero_in_base.find(q) != zero_in_base.end()) continue;

        set<TimedSpec> specs = wl.get_all_specs();
        for (set<TimedSpec>::iterator it = specs.begin(); it != specs.end(); it++) {
            candidate = wl;

            wl_spec_t wl_spec = it->get_wl_spec();
            if (!holds_alternative<Comp>(wl_spec)) continue;
            Comp comp = get<Comp>(wl_spec);
            rhs_t rhs = comp.get_rhs();

            if (holds_alternative<unsigned int>(rhs) && get<unsigned int>(rhs) == 0 &&
                comp.get_op() == op_t::LE)
                continue;

            lhs_t lhs = comp.get_lhs();
            lhs_t new_lhs = lhs;

            if (holds_alternative<Indiv>(lhs)) {
                Indiv tone = get<Indiv>(lhs);

                // TODO: aggregatable
                if (tone.get_metric() != metric_t::CENQ) continue;

                qset_t new_qset;
                new_qset.insert(q);
                new_qset.insert(tone.get_queue());
                new_lhs = QSum(new_qset, tone.get_metric());
            } else if (holds_alternative<QSum>(lhs)) {
                QSum tsum = get<QSum>(lhs);

                // TODO: aggregatable
                if (tsum.get_metric() != metric_t::CENQ) continue;
                qset_t new_qset = tsum.get_qset();
                new_qset.insert(q);
                new_lhs = QSum(new_qset, tsum.get_metric());
            }

            Comp n_comp = Comp(new_lhs, comp.get_op(), comp.get_rhs());
            TimedSpec new_spec = TimedSpec(n_comp, it->get_time_range(), total_time);

            candidate.mod_spec(*it, new_spec);

            if (candidate == wl || candidate.is_empty() || candidate.is_all()) continue;

            DEBUG_MSG(candidate << endl);
            bool is_ans = check(candidate, "aggregate_indivs_to_sums");
            if (is_ans) {
                wl = candidate;
                opt_count["aggregate_indivs_to_sums"]++;
            }
        }
    }

    after_cost["aggregate_indivs_to_sums"].push_back(cost(wl, "aggregate_indivs_to_sums"));
    return wl;
}

void Search::run() {
    DEBUG_MSG("target queues: " << target_queues << endl);
    time_typ start = noww();

    cp->set_query(query);

    Workload wl(max_spec, in_queue_cnt, total_time);
    search(wl);

    print_stats();

    cout << "search time: " << (get_diff_millisec(start, noww()) / 1000.0) << " s" << endl;

    for (unsigned int i = 0; i < solutions.size(); i++) {
        cout << "Solution " << i << endl;
        Workload refined_ans = refine(solutions[i]);
        cout << refined_ans << endl << endl;
    }

    // cout << "total Search::run time: " << get_diff_sec(start, noww()) << endl;
}


unsigned int Search::good_example_match_count(Workload wl) {
    unsigned int res = 0;
    for (deque<IndexedExample*>::iterator it = good_examples.begin(); it != good_examples.end();
         it++) {
        bool satisfies = cp->workload_satisfies_example(wl, *it);

        if (satisfies) res++;
    }
    return res;
}

unsigned int Search::bad_example_match_count(Workload wl) {
    unsigned int res = 0;
    for (deque<IndexedExample*>::iterator it = bad_examples.begin(); it != bad_examples.end();
         it++) {
        bool satisfies = cp->workload_satisfies_example(wl, *it);

        if (satisfies) res++;
    }
    return res;
}

unsigned int Search::cost(Workload wl, string function_name) {
    call_cnt++;
    auto start = noww();

    unsigned int example_cost = 0;

    example_cost += (good_examples.size() - good_example_match_count(wl)) * GOOD_EXAMPLE_WEIGHT;
    // example_cost += good_example_match_count(wl) * GOOD_EXAMPLE_WEIGHT;
    example_cost += (bad_example_match_count(wl) * BAD_EXAMPLE_WEIGHT);

    wl_cost_t wl_cost = wl.cost();

    cost_t res(example_cost, wl_cost);

    if(sum_call_time.find(function_name) == sum_call_time.end())
        sum_call_time[function_name] = get_diff_millisec(start, noww());
    else
        sum_call_time[function_name] += get_diff_millisec(start, noww());
    return uint_val(res);
}

void Search::pick_neighbors(Workload wl, vector<Workload>& neighbors) {
    while (neighbors.size() == 0) {

        // DEBUG_MSG("add " << endl);
        //  Adding a random sub-formula
        //  TODO: check if next candidate is not empty (or initial wl)?
        if (wl.size() < max_spec) {
            for (unsigned int i = 0; i < RANDOM_ADD_CNT; i++) {
                TimedSpec spec = spec_factory.random_timed_spec();
                Workload candidate_neighbor = wl;
                candidate_neighbor.add_spec(spec);
                if (!(candidate_neighbor.is_empty() || candidate_neighbor.is_all() ||
                      candidate_neighbor == wl)) {
                    //                    DEBUG_MSG(candidate_neighbor << endl);
                    neighbors.push_back(candidate_neighbor);
                }
            }
        }

        // DEBUG_MSG("rm" << endl);
        //  Removing a sub-formula
        if (wl.size() > 0) {
            set<TimedSpec> wl_specs = wl.get_all_specs();
            for (set<TimedSpec>::iterator it = wl_specs.begin(); it != wl_specs.end(); it++) {
                TimedSpec to_remove = *it;

                Workload candidate_neighbor = wl;

                candidate_neighbor.rm_spec(to_remove);
                if (!(candidate_neighbor.is_empty() || candidate_neighbor.is_all() ||
                      candidate_neighbor == wl)) {
                    //                    DEBUG_MSG(candidate_neighbor << endl);
                    neighbors.push_back(candidate_neighbor);
                }
            }
        }

        // DEBUG_MSG("replace" << endl);
        //  Replace a sub-formual with a random sub-formula
        if (wl.size() > 0) {
            set<TimedSpec> wl_specs = wl.get_all_specs();
            for (set<TimedSpec>::iterator it = wl_specs.begin(); it != wl_specs.end(); it++) {
                TimedSpec to_replace = *it;
                TimedSpec spec = spec_factory.random_timed_spec();

                Workload candidate_neighbor = wl;

                candidate_neighbor.mod_spec(to_replace, spec);
                if (!(candidate_neighbor.is_empty() || candidate_neighbor.is_all() ||
                      candidate_neighbor == wl)) {
                    //                    DEBUG_MSG(candidate_neighbor << endl);
                    neighbors.push_back(candidate_neighbor);
                }
            }
        }

        // DEBUG_MSG("mod" << endl);
        //  Modifying a sub-formula
        if (wl.size() > 0) {
            unsigned int new_neighbors = 0;
            for (unsigned int tries = 0; tries <= 5 && new_neighbors == 0; tries++) {
                set<TimedSpec> wl_specs = wl.get_all_specs();
                unsigned int ind = 0;
                for (set<TimedSpec>::iterator it = wl_specs.begin(); it != wl_specs.end(); it++) {
                    TimedSpec to_modify = *it;

                    vector<TimedSpec> modified_specs;
                    spec_factory.pick_neighbors(to_modify, modified_specs);

                    for (unsigned int j = 0; j < modified_specs.size(); j++) {
                        Workload candidate_neighbor = wl;

                        candidate_neighbor.mod_spec(to_modify, modified_specs[j]);

                        if (!(candidate_neighbor.is_empty() || candidate_neighbor.is_all() ||
                              candidate_neighbor == wl)) {
                            neighbors.push_back(candidate_neighbor);
                            //                            DEBUG_MSG(candidate_neighbor << endl);
                            new_neighbors++;
                        }
                    }

                    ind++;
                }
            }
        }
    }
}

Workload Search::random_neighbor(Workload wl, unsigned int hops) {
    unsigned int wl_cost = cost(wl);

    vector<Workload> neighbors;
    pick_neighbors(wl, neighbors);


    unsigned long start_ind = 0;
    vector<Workload> next_hop_neighbors;
    vector<Workload> eligible_next_hop_neighbors;

    for (unsigned int h = 1; h < hops && neighbors.size() < MAX_CANDIDATES; h++) {

        //        DEBUG_MSG("hop: " << h << endl);
        unsigned long next_ind = neighbors.size();

        for (unsigned long c = start_ind; c < next_ind; c++) {
            pick_neighbors(neighbors.at(c), next_hop_neighbors);

            for (unsigned int j = 0; j < next_hop_neighbors.size(); j++) {
                Workload new_nei = next_hop_neighbors.at(j);

                if (new_nei == wl) {
                    continue;
                }

                bool was_found_infeasible = false;
                for (unsigned int iind = 0; iind < infeasible_wls.size(); iind++) {
                    if (infeasible_wls[iind] == new_nei) {
                        was_found_infeasible = true;
                        break;
                    }
                }
                if (was_found_infeasible) {
                    continue;
                }

                eligible_next_hop_neighbors.push_back(new_nei);
            }

            neighbors.insert(neighbors.end(),
                             eligible_next_hop_neighbors.begin(),
                             eligible_next_hop_neighbors.end());

            // clear tmp vectors
            next_hop_neighbors.clear();
            eligible_next_hop_neighbors.clear();

            if (neighbors.size() > MAX_CANDIDATES) {
                //                DEBUG_MSG("broke at hop " << h << " at " << c << " in [" <<
                //                start_ind << ", " << next_ind << "]" << endl);
                break;
            }
        }

        start_ind = next_ind;
    }

    DEBUG_MSG("picking the candidate from set of " << neighbors.size() << endl);

    vector<Workload> candidates;
    vector<Workload> greater_cost;

    for (unsigned int i = 0; i < neighbors.size(); i++) {
        Workload nei = neighbors[i];
        unsigned int nei_cost = cost(nei);
        if (nei_cost < wl_cost) {
            candidates.push_back(nei);
        } else {
            greater_cost.push_back(nei);
        }
    }

    if (candidates.size() == 0) {
        candidates = neighbors;
    }

    uniform_int_distribution<unsigned int> nei_dist(0, (unsigned int) candidates.size() - 1);

    DEBUG_MSG("done picking the candidate" << endl);
    mt19937& gen = dists->get_gen();
    return candidates[nei_dist(gen)];
}

void Search::print_stats() {
    cout << "-------------------- STATS -----------------------" << endl;
//    cout << "infeasible_input_cnt: " << infeasible_input_cnt << endl;

    // Get collection of all keys
    set<string> all_keys;
    // sum_check_time
    for(auto it = sum_check_time.begin(); it != sum_check_time.end(); ++it){
        all_keys.insert(it->first);
    }
    // max_check_time
    for(auto it = max_check_time.begin(); it != max_check_time.end(); ++it){
        all_keys.insert(it->first);
    }
    // no_solver_call
    for(auto it = no_solver_call.begin(); it != no_solver_call.end(); ++it){
        all_keys.insert(it->first);
    }
    // input_only_solver_call
    for(auto it = input_only_solver_call.begin(); it != input_only_solver_call.end(); ++it){
        all_keys.insert(it->first);
    }
    // query_only_solver_call
    for(auto it = query_only_solver_call.begin(); it != query_only_solver_call.end(); ++it){
        all_keys.insert(it->first);
    }
    // full_solver_call
    for(auto it = full_solver_call.begin(); it != full_solver_call.end(); ++it){
        all_keys.insert(it->first);
    }
    // sum_call_time
    for(auto it = sum_call_time.begin(); it != sum_call_time.end(); ++it){
        all_keys.insert(it->first);
    }

    std::map<string, double> avg_check_time;
    std::map<string, double> avg_call_time;

    std::map<string, double> avg_cost_improvement_abs;
    std::map<string, double> avg_cost_improvement_rel;

    // For every key-value pair
    for(auto it = all_keys.begin(); it != all_keys.end(); ++it){
        cout << "Function: " << *it << endl;
//        cout << "number of rounds: " << round_no << endl;
//        cout << "rounds in local search: " << rounds_in_local_search << endl;
//        cout << "number of resets: " << reset_cnt << endl;
        if(no_solver_call.find(*it) == no_solver_call.end()) no_solver_call[*it] = 0;
        cout << "no_solver_call: " << no_solver_call[*it] << endl;
        if(input_only_solver_call.find(*it) == input_only_solver_call.end()) input_only_solver_call[*it] = 0;
        cout << "input_only_solver_call: " << input_only_solver_call[*it] << endl;
        if(query_only_solver_call.find(*it) == query_only_solver_call.end()) query_only_solver_call[*it] = 0;
        cout << "query_only_solver_call: " << query_only_solver_call[*it] << endl;
        if(full_solver_call.find(*it) == full_solver_call.end()) full_solver_call[*it] = 0;
        cout << "full_solver_call: " << full_solver_call[*it] << endl;
        cout << "opt_count: " << opt_count[*it] << endl;

        // Calculate cost improvements using before_cost and after_cost
        if(before_cost.find(*it) == before_cost.end()) before_cost[*it] = {};
        if(after_cost.find(*it) == after_cost.end()) after_cost[*it] = {};
        avg_cost_improvement_abs[*it] = 0;
        avg_cost_improvement_rel[*it] = 0;
        for(int i = 0; i < before_cost[*it].size(); i++){
            avg_cost_improvement_abs[*it] += (after_cost[*it][i] - before_cost[*it][i]);
            avg_cost_improvement_rel[*it] += (after_cost[*it][i] - before_cost[*it][i]) / before_cost[*it][i];
        }
        if(before_cost[*it].size() == 0) before_cost[*it].push_back(1); // Avoid division by zero
        avg_cost_improvement_abs[*it] /= before_cost[*it].size();
        avg_cost_improvement_rel[*it] /= before_cost[*it].size();
        cout << "avg_cost_improvement_abs: " << avg_cost_improvement_abs[*it] << endl;
        cout << "avg_cost_improvement_rel: " << avg_cost_improvement_rel[*it] << endl;


        if(full_solver_call[*it] == 0) full_solver_call[*it] = 1; // For average calculations
        cout << "Timing Stats: " << endl;
        if(sum_check_time.find(*it) == sum_check_time.end()) sum_check_time[*it] = 0;
        cout << "sum_check_time: " << sum_check_time[*it] << "ms" << endl;
        if(max_check_time.find(*it) == max_check_time.end()) max_check_time[*it] = 0;
        avg_check_time[*it] = sum_check_time[*it] / (double) full_solver_call[*it];
        cout << "avg_check_time: " << avg_check_time[*it] << "ms" << endl;
        cout << "max_check_time: " << max_check_time[*it] << "ms" << endl;
        if(sum_call_time.find(*it) == sum_call_time.end()) sum_call_time[*it] = 0;
        cout << "sum_call_time: " << sum_call_time[*it] << "ms" << endl;
        avg_call_time[*it] = sum_call_time[*it] / (double) full_solver_call[*it];
        cout << "avg_call_time: " << avg_call_time[*it] << "ms" << endl;

//        cout << "full solver stats:\n";
//        cout << "   w/o query: " << (cp->get_check_workload_without_query_avg_time() / 1000.0) << " "
//             << (cp->get_check_workload_without_query_max_time() / 1000.0) << endl;
//        cout << "   w/ query: " << (cp->get_check_workload_with_query_avg_time() / 1000.0) << " "
//             << (cp->get_check_workload_with_query_max_time() / 1000.0) << endl;
//        if(no_solver_call.find(it) == no_solver_call.end()) no_solver_call[it] = 0;
//        cout << round_no << "\t" << reset_cnt << "\t" << rounds_in_local_search << "\t"
//             << no_solver_call[it] << "\t" << full_solver_call[it] << "\t"
//             << cp->get_check_workload_without_query_avg_time() << " ("
//             << cp->get_check_workload_without_query_max_time() << ")"
//             << "\t" << (full_solver_call[it] - infeasible_input_cnt) << "\t"
//             << cp->get_check_workload_with_query_avg_time() << " ("
//             << cp->get_check_workload_with_query_max_time() << ")"
//             << "\t" << endl;
        cout << "--------------------------------------------------" << endl;
    }

    // Save output to CSV
    string example_name = globalArgs[0];

    // Define the file and directory paths using filesystem
    namespace fs = std::filesystem;
    fs::path dirPath = "benchmarks/csv";
    fs::path filePath = dirPath / (example_name + ".csv");

    // Output the current working directory for debugging
    cout << "Current working directory: " << fs::current_path() << endl;

    // Check if the directory exists, create if not
    if (!fs::exists(dirPath)) {
        cout << "Creating directory: " << dirPath << endl;
        fs::create_directories(dirPath); // This creates all directories in the path if they don't exist
    }

    // Open the file
    ofstream myfile(filePath, ios::out); // Open for appending
    if (myfile.fail()) {
        cerr << "Failed to open file: " << filePath << endl;
        exit(1); // Exit if fail to open
    }
    cout << "Saving to " << filePath << endl;

    // Write data to file
    myfile << "Function, no_solver_call, input_only_solver_call, query_only_solver_call, full_solver_call, opt_count, avg_cost_improvement_abs, avg_cost_improvement_rel, sum_check_time, avg_check_time, max_check_time, sum_call_time, avg_call_time" << endl;
    for(auto it = all_keys.begin(); it != all_keys.end(); ++it){
        myfile << *it << ", " << no_solver_call[*it] << ", " << input_only_solver_call[*it] << ", " << query_only_solver_call[*it] << ", " << full_solver_call[*it] << ", " << opt_count[*it] << ", " << avg_cost_improvement_abs[*it] << ", " << avg_cost_improvement_rel[*it] << ", " << sum_check_time[*it] << ", " << avg_check_time[*it] << ", " << max_check_time[*it] << ", " << sum_call_time[*it] << ", " << avg_call_time[*it] << endl;
    }
    myfile.close();
}
