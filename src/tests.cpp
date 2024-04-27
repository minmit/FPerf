//
//  tests.cpp
//  FPerf
//
//  Created by Mina Tahmasbi Arashloo on 12/05/22.
//  Copyright © 2022 Mina Tahmasbi Arashloo. All rights reserved.
//

#include "tests.hpp"

#include "buggy_2l_rr_scheduler.hpp"
#include "leaf_spine.hpp"
#include "loom_mqprio.hpp"
#include "params.hpp"
#include "priority_scheduler.hpp"
#include "query.hpp"
#include "rr_scheduler.hpp"
#include "search.hpp"
#include "tbf.hpp"
#include "aipg.hpp"
#include "ecmp.hpp"
#include "dst.hpp"
#include "queue.hpp"
#include "net_context.hpp"
#include "metric.hpp"

#include "global_vars.h"
std::map<std::string, unsigned int> opt_count;
std::map<std::string, vector<unsigned int>> before_cost;
std::map<std::string, vector<unsigned int>> after_cost;

#include <iterator>
#include <limits>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <vector>
#include <chrono>
#include <unordered_set>

void run(ContentionPoint* cp,
         IndexedExample* base_eg,
         unsigned int good_example_cnt,
         string good_examples_file,
         unsigned int bad_example_cnt,
         string bad_examples_file,
         Query& query,
         unsigned int max_spec,
         SharedConfig* config);


std::vector<std::set<unsigned int>> find_contiguous_sets(const std::set<unsigned int>& input_set) {
    std::vector<std::set<unsigned int>> contiguous_sets;
    std::set<unsigned int> current_set;

    auto it = input_set.begin();
    unsigned int previous = *it;
    current_set.insert(previous);

    for (it = std::next(it); it != input_set.end(); ++it) {
        // If the current element is consecutive, add it to the current set
        if (*it == previous + 1) {
            current_set.insert(*it);
        } else {
            // If not consecutive, start a new set
            contiguous_sets.push_back(current_set);
            current_set.clear();
            current_set.insert(*it);
        }
        previous = *it;
    }

    // Add the last set
    if (!current_set.empty()) {
        contiguous_sets.push_back(current_set);
    }

    // Remove sets of size 1
    contiguous_sets.erase(std::remove_if(contiguous_sets.begin(), contiguous_sets.end(), [](const std::set<unsigned int>& s){return s.size() == 1;}), contiguous_sets.end());

    return contiguous_sets;
}

Workload combine(Workload wl, Search& search) { // Need to pass in search so we can check if the new spec is valid
    // --- PATTERN 1 ---
    //    // EXAMPLE: Need a systematic process which would turn a wl containing the following specs:
    ////    [1, 1]: SUM_[q in {0, 1, }] cenq(q ,t) = 1
    ////    [2, 2]: SUM_[q in {0, 1, }] cenq(q ,t) = 2
    ////    [3, 3]: SUM_[q in {0, 1, }] cenq(q ,t) = 3
    ////    [4, 4]: SUM_[q in {0, 1, }] cenq(q ,t) = 4
    ////    [5, 5]: SUM_[q in {0, 1, }] cenq(q ,t) = 5
    ////    [6, 6]: SUM_[q in {0, 1, }] cenq(q ,t) = 6
    //// Into the following:
    ////    [1, 6]: SUM_[q in {0, 1, }] cenq(q ,t) = t
    //// Which expresses the exact same information, but is more concise.
    //
    //    // In general: If we find a set of specs of the form:
    //    // [t1, t1]: (SUM_[q in Q] OR INDIV(q)) cenq(q ,t) = c1
    //    // [t2, t2]: (SUM_[q in Q] OR INDIV(q)) cenq(q ,t) = c2
    //    // ...
    //    // [tn, tn]: (SUM_[q in Q] OR INDIV(q)) cenq(q ,t) = cn
    //    // Where t1 < t2 < ... < tn, and c1 < c2 < ... < cn and they are all either SUM over the same set of queues or INDIV over the same queue
    //    // [t1, tn]: (SUM_[q in Q] OR INDIV(q)) cenq(q ,t) = t
    //
    //    // Code:
    //    // Get all specs
    //    set<TimedSpec> specs = wl.get_all_specs();
    //    // For each spec, check if it's of the form [t, t]: cenq(q, t) = c where c = t
    //    // If so, we need to keep track of: q, c, t

    // Step 1: Find all specs of the form [t, t]: cenq(q, t) = c where c = t

    set<TimedSpec> specs = wl.get_all_specs();

    // Keep track of sets (of size 1 if INDIV, of size>1 if SUM) of q's to t's

    std::map<std::set<unsigned int>, std::set<unsigned int>> qset_to_tset;

    for (const TimedSpec& spec : specs) {
        if (spec.get_time_range().first == spec.get_time_range().second) {
            wl_spec_t wl_spec = spec.get_wl_spec();
            if (holds_alternative<Comp>(wl_spec)) {
                Comp comp = get<Comp>(wl_spec);
                if(holds_alternative<unsigned int>(comp.rhs)){
                    unsigned int c = get<unsigned int>(comp.rhs);
                    unsigned int t = spec.get_time_range().first + 1; // +1 because time starts at 1
                    if (holds_alternative<Indiv>(comp.lhs)) {
                        Indiv indiv = get<Indiv>(comp.lhs);
                        unsigned int q = indiv.queue;
                        if (t == c && indiv.get_metric() == metric_t::CENQ) {
                            // Add this spec to the set of specs for this q
                            qset_to_tset[{q}].insert(t);
                        }
                    } else if (holds_alternative<QSum>(comp.lhs)) {
                        QSum qsum = get<QSum>(comp.lhs);
                        std::set<unsigned int> qset = qsum.qset;
                        if (t == c && qsum.get_metric() == metric_t::CENQ) {
                            // Add this spec to the set of specs for this qset
                            qset_to_tset[qset].insert(t);
                        }
                    }
                }
            }
        }
    }

    //    cout << "qset_to_tset: " << endl;
    //    for(auto const& [qset, tset] : qset_to_tset){
    //        cout << "qset: " << qset << ", tset: " << tset << endl;
    //    }

    // Step 2: For each set of t's, merge the consecutive ones into a single spec
    for(auto const& [qset, tset] : qset_to_tset) {
        // For each qset, we need to check if there is a set of c's that are consecutive
        // If so, we check if the corresponding set of t's are consecutive
        // If so, we can replace the set of specs with a single spec
        // We can do this by removing all the old specs and adding a new spec
        // Implementation:
        // Check if tset is consecutive
        std::vector<std::set<unsigned int>> contiguous_tsets = find_contiguous_sets(tset);
        for (auto const &contiguous_tset: contiguous_tsets) {
            // Merge into one spec
            // Step 1: Add a new spec (QSum or Indiv depending on whether qset has size 1 or not) of form [t1, tn]: (SUM_[q in Q] OR INDIV(q)) cenq(q ,t) = t
            if (qset.size() == 1) {
                TimedSpec spec_to_add = TimedSpec(Comp(Indiv(metric_t::CENQ, *qset.begin()), op_t::EQ, Time(1)), time_range_t(*contiguous_tset.begin() - 1, *contiguous_tset.rbegin() - 1), wl.get_total_time());
                //                cout << "BROADEN: Adding spec: " << spec_to_add << endl;
                wl.add_spec(spec_to_add);
                opt_count["combine"]++;
            } else {
                TimedSpec spec_to_add = TimedSpec(Comp(QSum(qset, metric_t::CENQ), op_t::EQ, Time(1)), time_range_t(*contiguous_tset.begin() - 1, *contiguous_tset.rbegin() - 1), wl.get_total_time());
                //                cout << "BROADEN: Adding spec: " << spec_to_add << endl;
                wl.add_spec(spec_to_add);
                opt_count["combine"]++;
            }

            // Step 2: Remove all the old specs
            for (auto const &t : contiguous_tset) {
                if (qset.size() == 1) {
                    // This spec is part of the contiguous set of t's
                    // Remove it
                    TimedSpec spec_to_remove = TimedSpec(Comp(Indiv(metric_t::CENQ, *qset.begin()), op_t::EQ, t), time_range_t(t - 1, t - 1), wl.get_total_time());
                    //                    cout << "BROADEN: Removing spec: " << spec_to_remove << endl;
                    wl.rm_spec(spec_to_remove);
                } else {
                    // This spec is part of the contiguous set of t's
                    // Remove it
                    TimedSpec spec_to_remove = TimedSpec(Comp(QSum(qset, metric_t::CENQ), op_t::EQ, t), time_range_t(t - 1, t - 1), wl.get_total_time());
                    //                    cout << "BROADEN: Removing spec: " << spec_to_remove << endl;
                    wl.rm_spec(spec_to_remove);
                }
            }
        }
    }

// --- PATTERN 2 ---
    // If we have a set of specs of the form:
    // [t1, t2]: (INDIV(q1) cenq(q, t) = 0
    // [t1, t2]: (INDIV(q2) cenq(q, t) = 0
    // [t1, t2]: (INDIV(q3) cenq(q, t) = 0
    // ...
    // [t1, t2]: (INDIV(qn) cenq(q, t) = 0

    // We can replace it with a single spec:
    // [t1, t2]: (SUM_[q in Q] cenq(q, t) = 0
    // Because if the sum of all the cenq's is 0, then each individual cenq is 0 (they can't be negative)

    specs = wl.get_all_specs();

    // Filter out all QSum, non-Comp, or non-CENQ(q, t) == 0 specs
    std::set<TimedSpec> indiv_specs;
    for (const TimedSpec& spec : specs) {
        wl_spec_t wl_spec = spec.get_wl_spec();
        if (holds_alternative<Comp>(wl_spec)) {
            Comp comp = get<Comp>(wl_spec);
            if (holds_alternative<Indiv>(comp.lhs)) {
                Indiv indiv = get<Indiv>(comp.lhs);
                if (indiv.get_metric() == metric_t::CENQ){
                    if(holds_alternative<unsigned int>(comp.rhs) && get<unsigned int>(comp.rhs) == 0){
                        // Check if operation is = or <= 0
                        if (comp.get_op() == op_t::EQ || comp.get_op() == op_t::LE) {
                            indiv_specs.insert(spec);
                        }
                    }
                }
            }
        }
    }

//    cout << "indiv_specs: " << endl;
//    for (const TimedSpec& spec : indiv_specs) {
//        cout << spec << endl;
//    }

    // Keep track of sets of q's to TimedSpecs
    std::map<time_range_t, std::set<TimedSpec>> tset_to_qset;

    for (const TimedSpec& spec : indiv_specs) {
        time_range_t time_range = spec.get_time_range();
        wl_spec_t wl_spec = spec.get_wl_spec();
        tset_to_qset[time_range].insert(spec);
    }

    // for each mapping in tset_to_qset, if the set of TimedSpecs corresponding to a time_range has size greater than 1, we replace it with a single spec
    for(auto const& [time_range, TimedSpecs] : tset_to_qset) {
        if (TimedSpecs.size() > 1) {
            // Merge into one spec

            // Step 1: Remove all the old specs
            for (auto const &spec : TimedSpecs) {
                // Remove it
                wl.rm_spec(spec);
            }

            // Step 2: Add a new spec of form [t1, t2]: (SUM_[q in Q] cenq(q ,t) <= 0

            // Collect all q's
            std::set<unsigned int> qset;
            for (const TimedSpec& spec : TimedSpecs) {
                wl_spec_t wl_spec = spec.get_wl_spec();
                Comp comp = get<Comp>(wl_spec);
                Indiv indiv = get<Indiv>(comp.lhs);
                unsigned int q = indiv.queue;
                qset.insert(q);
            }

            TimedSpec spec_to_add = TimedSpec(Comp(QSum(qset, metric_t::CENQ), op_t::LE, Time(0)), time_range, wl.get_total_time());
            //            cout << "BROADEN: Adding spec: " << spec_to_add << endl;
            wl.add_spec(spec_to_add);
            opt_count["combine"]++;
        }
    }


    return wl;
}

Workload broaden_operations(Workload wl, Search& search) {
    // For COMP specs, try to broaden operations (change = to <=, >=, change < to <=, change > to >=) and see if it's still valid
    // If so, replace the spec with the new spec
    // If not, keep the old spec
    before_cost["broaden_operations"].push_back(search.cost(wl, "broaden_operations"));
    set<TimedSpec> specs = wl.get_all_specs();
    specs = wl.get_all_specs();
    for (const TimedSpec& spec : specs) {
        wl_spec_t wl_spec = spec.get_wl_spec();
        if (holds_alternative<Comp>(wl_spec)) {
            Comp comp = get<Comp>(wl_spec);
            if (holds_alternative<Indiv>(comp.lhs)) {
                Indiv indiv = get<Indiv>(comp.lhs);
                unsigned int q = indiv.queue;

                // Get metric
                metric_t metric = indiv.get_metric();

                // Try to broaden the op
                op_t op = comp.get_op();
                if (op == op_t::EQ) {
                    // Try <=
                    TimedSpec spec_to_try = TimedSpec(Comp(Indiv(metric, q), op_t::LE, comp.rhs),
                                                      spec.get_time_range(), wl.get_total_time());
                    // Create temp workload
                    Workload temp_wl = wl;
                    temp_wl.rm_spec(spec);
                    temp_wl.add_spec(spec_to_try);
                    if (search.check(temp_wl, "broaden_operations")) {
                        // It's valid, replace the spec
                        wl.rm_spec(spec);
                        wl.add_spec(spec_to_try);
                        opt_count["broaden_operations"]++;
                        continue;
                    } else {
                        // It's not valid, keep the old spec
                        // TODO: Debug fq_codel using counter-example?
                    }

                    // Try >=
                    spec_to_try = TimedSpec(Comp(Indiv(metric, q), op_t::GE, comp.rhs),
                                            spec.get_time_range(), wl.get_total_time());
                    // Create temp workload
                    temp_wl = wl;
                    temp_wl.rm_spec(spec);
                    temp_wl.add_spec(spec_to_try);
                    if (search.check(temp_wl, "broaden_operations")) {
                        // It's valid, replace the spec
                        wl.rm_spec(spec);
                        wl.add_spec(spec_to_try);
                        opt_count["broaden_operations"]++;
                        continue;
                    } else {
                        // It's not valid, keep the old spec
                    }
                } else if (op == op_t::LT) {
                    // Try <=
                    TimedSpec spec_to_try = TimedSpec(Comp(Indiv(metric, q), op_t::LE, comp.rhs),
                                                      spec.get_time_range(), wl.get_total_time());
                    // Create temp workload
                    Workload temp_wl = wl;
                    temp_wl.rm_spec(spec);
                    temp_wl.add_spec(spec_to_try);
                    if (search.check(temp_wl, "broaden_operations")) {
                        // It's valid, replace the spec
                        wl.rm_spec(spec);
                        wl.add_spec(spec_to_try);
                        opt_count["broaden_operations"]++;
                        continue;
                    } else {
                        // It's not valid, keep the old spec
                    }
                } else if (op == op_t::GT) {
                    // Try >=
                    TimedSpec spec_to_try = TimedSpec(Comp(Indiv(metric, q), op_t::GE, comp.rhs),
                                                      spec.get_time_range(), wl.get_total_time());
                    // Create temp workload
                    Workload temp_wl = wl;
                    temp_wl.rm_spec(spec);
                    temp_wl.add_spec(spec_to_try);
                    if (search.check(temp_wl, "broaden_operations")) {
                        // It's valid, replace the spec
                        wl.rm_spec(spec);
                        wl.add_spec(spec_to_try);
                        opt_count["broaden_operations"]++;
                        continue;
                    } else {
                        // It's not valid, keep the old spec
                    }
                }
            } else if (holds_alternative<QSum>(comp.lhs)) {
                QSum qsum = get<QSum>(comp.lhs);
                std::set<unsigned int> qset = qsum.qset;

                // Get metric
                metric_t metric = qsum.get_metric();

                // Try to broaden the op
                op_t op = comp.get_op();
                if (op == op_t::EQ) {
                    // Try <=
                    TimedSpec spec_to_try = TimedSpec(Comp(QSum(qset, metric),
                                                           op_t::LE,
                                                           comp.rhs),
                                                      spec.get_time_range(),
                                                      wl.get_total_time());
                    // Create temp workload
                    Workload temp_wl = wl;
                    temp_wl.rm_spec(spec);
                    temp_wl.add_spec(spec_to_try);
                    if (search.check(temp_wl, "broaden_operations")) {
                        // It's valid, replace the spec
                        wl.rm_spec(spec);
                        wl.add_spec(spec_to_try);
                        opt_count["broaden_operations"]++;
                        continue;
                    } else {
                        // It's not valid, keep the old spec
                    }

                    // Try >=
                    spec_to_try = TimedSpec(Comp(QSum(qset, metric),
                                                 op_t::GE,
                                                 comp.rhs),
                                            spec.get_time_range(),
                                            wl.get_total_time());
                    // Create temp workload
                    temp_wl = wl;
                    temp_wl.rm_spec(spec);
                    temp_wl.add_spec(spec_to_try);
                    if (search.check(temp_wl, "broaden_operations")) {
                        // It's valid, replace the spec
                        wl.rm_spec(spec);
                        wl.add_spec(spec_to_try);
                        opt_count["broaden_operations"]++;
                        continue;
                    } else {
                        // It's not valid, keep the old spec
                    }
                } else if (op == op_t::LT) {
                    // Try <=
                    TimedSpec spec_to_try = TimedSpec(Comp(QSum(qset, metric),
                                                           op_t::LE,
                                                           comp.rhs),
                                                      spec.get_time_range(),
                                                      wl.get_total_time());
                    // Create temp workload
                    Workload temp_wl = wl;
                    temp_wl.rm_spec(spec);
                    temp_wl.add_spec(spec_to_try);
                    if (search.check(temp_wl, "broaden_operations")) {
                        // It's valid, replace the spec
                        wl.rm_spec(spec);
                        wl.add_spec(spec_to_try);
                        opt_count["broaden_operations"]++;
                        continue;
                    } else {
                        // It's not
                        // valid, keep the old spec
                    }
                } else if (op == op_t::GT) {
                    // Try >=
                    TimedSpec spec_to_try = TimedSpec(Comp(QSum(qset, metric),
                                                           op_t::GE,
                                                           comp.rhs),
                                                      spec.get_time_range(),
                                                      wl.get_total_time());
                    // Create temp workload
                    Workload temp_wl = wl;
                    temp_wl.rm_spec(spec);
                    temp_wl.add_spec(spec_to_try);
                    if (search.check(temp_wl, "broaden_operations")) {
                        // It's valid, replace the spec
                        wl.rm_spec(spec);
                        wl.add_spec(spec_to_try);
                        opt_count["broaden_operations"]++;
                        continue;
                    } else {
                        // It's not valid, keep the old spec
                    }
                }
            }
        }
    }

    after_cost["broaden_operations"].push_back(search.cost(wl, "broaden_operations"));
    return wl;
}

Workload restrict_time_ranges(Workload wl, Search& search) {
    auto start_time = std::chrono::high_resolution_clock::now();
    before_cost["restrict_time_ranges"].push_back(search.cost(wl, "restrict_time_ranges"));

    // For each spec, try to restrict the time range and see if it's still valid
    // If so, replace the spec with the new spec
    // If not, keep the old spec
    set<TimedSpec> specs = wl.get_all_specs();
    for (const TimedSpec& spec : specs) {
        wl_spec_t wl_spec = spec.get_wl_spec();
        time_range_t time_range = spec.get_time_range();

        // Get set of all specs except the current one
        set<TimedSpec> other_specs = wl.get_all_specs();
        other_specs.erase(spec);

        // Try to restrict right side of time range
        while(time_range.second > time_range.first){
            time_range.second--;
            TimedSpec spec_to_try = TimedSpec(wl_spec, time_range, wl.get_total_time());
            // Create temp workload
            Workload temp_wl = wl;
            temp_wl.clear();
            for(const TimedSpec& other_spec : other_specs){
                temp_wl.add_spec(other_spec);
            }
            temp_wl.add_spec(spec_to_try);
            if (search.check(temp_wl, "restrict_time_ranges")) {
                // It's valid, replace the spec
                wl = temp_wl;
                opt_count["restrict_time_ranges"]++;
//                cout << "spec before restricting second time range: " << spec << endl;
//                cout << "spec after restricting second time range: " << spec_to_try << endl;
                continue;
            } else {
                // It's not valid, keep the old spec
                time_range.second++;
                break;
            }
        }

        // Try to restrict left side of time range
        while(time_range.first < time_range.second){
            time_range.first++;
            TimedSpec spec_to_try = TimedSpec(wl_spec, time_range, wl.get_total_time());
            // Create temp workload
            Workload temp_wl = wl;
            temp_wl.clear();
            for(const TimedSpec& other_spec : other_specs){
                temp_wl.add_spec(other_spec);
            }
            temp_wl.add_spec(spec_to_try);
            if (search.check(temp_wl, "restrict_time_ranges")) {
                // It's valid, replace the spec
                wl = temp_wl;
                opt_count["restrict_time_ranges"]++;
//                cout << "spec before restricting second time range: " << spec << endl;
//                cout << "spec after restricting second time range: " << spec_to_try << endl;
                continue;
            } else {
                // It's not valid, keep the old spec
                time_range.first--;
                break;
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    cout << "restrict_time_ranges took " << duration.count() << " milliseconds" << endl;

    after_cost["restrict_time_ranges"].push_back(search.cost(wl, "restrict_time_ranges"));
    return wl;
}

Workload restrict_time_ranges_z3(Workload wl, Search& search, ContentionPoint* cp) { // TEST: same as above, but using Z3 expressions
    auto start_time = std::chrono::high_resolution_clock::now();

    set<TimedSpec> specs = wl.get_all_specs();
    auto& ctx = cp->net_ctx.z3_ctx();
    auto& constraint_map = cp->constr_map;;

    for(const TimedSpec& spec : specs){
        cout << "Optimizing spec " << spec << endl;
//        auto opt = new optimize(ctx);
        auto opt = new solver(ctx);
        // FORALL enq, internal queue: !X || query
        // X = model && base_wl && workload_minus_current_spec && current_spec

        wl_spec_t wl_spec = spec.get_wl_spec();
        time_range_t time_range = spec.get_time_range();

        expr_vector enqs_and_internals(ctx);
        unsigned int total_time = wl.get_total_time();

        unsigned int bool_vars{0};
        unsigned int int_vars{0};

        // Add internal queue variables
        // None for prio

        expr start_time = ctx.int_const("start_time");
        expr end_time = ctx.int_const("end_time");

        // Model Constraints
        expr net_model = ctx.bool_val(true);
        for(auto const& [name, expr] : constraint_map){
            net_model = net_model && expr;
        }

        // Base Workload constraints
        expr base_wl = cp->base_wl_expr;

        // Workload constraints: add constraints for all specs except the current one (the one we're trying to restrict)
        expr workload_minus_current_spec = ctx.bool_val(true);
        for(const TimedSpec& other_spec : specs){
            if(other_spec != spec){
                expr other_spec_expr = cp->get_expr(other_spec);
                workload_minus_current_spec = workload_minus_current_spec && other_spec_expr;
            }
        }

        // Spec constraints
        // 1. Get expression for current spec
        Comp comp = get<Comp>(wl_spec);

        // 2. Add sanity-check constraints on new variables
        expr time_range_constraints = ctx.bool_val(true);
        time_range_constraints = time_range_constraints && (start_time <= end_time);
        time_range_constraints = time_range_constraints && (start_time >= ctx.int_val(0));
        time_range_constraints = time_range_constraints && (end_time < ctx.int_val(wl.get_total_time()));

        int current_start_time = time_range.first;
        int current_end_time = time_range.second;
        time_range_constraints = time_range_constraints && (start_time >= ctx.int_val(current_start_time));
        time_range_constraints = time_range_constraints && (end_time <= ctx.int_val(current_end_time));

        opt->add(time_range_constraints);

        // 3. Add constraints dependent on time range
        expr current_spec = ctx.bool_val(true);
        for(int i = 0; i < wl.get_total_time(); i++){
            expr i_expr = implies(start_time <= i && i <= end_time, cp->get_expr(comp, i));
            current_spec = current_spec && i_expr;
        }

        // Query constraints
        // !(current_constraints && !query)
        // i.e. it should not be possible to satisfy the current constraints and not satisfy the query
        expr query_expr = cp->query_expr;

        expr X = net_model && base_wl && workload_minus_current_spec && current_spec;
        expr not_X_or_query = !X || query_expr;

        auto all_vars = *cp->net_ctx.list_of_vars;

        // Print number of bool and int vars
        for(auto const& var : all_vars){
            if(var.is_bool()){
                bool_vars++;
            }else if(var.is_int()){
                int_vars++;
            }
        }
        cout << "bool_vars: " << bool_vars << ", int_vars: " << int_vars << endl;

        expr forall_not_X_or_query = forall(all_vars, not_X_or_query); // P(x)
        opt->add(forall_not_X_or_query);

        // Standard optimization problem min. f(x) s.t. P(x)
        // Can be encoded as logic formula P(x) && forall y: P(y) => f(x) <= f(y)
        // Here, f(end_time, start_time) is end_time - start_time
        // P(end_time, start_time) is !X || query_expr

        expr bounded_start_time = ctx.int_const("bounded_start_time");
        expr bounded_end_time = ctx.int_const("bounded_end_time");

        expr bounded_time_range_constraints = ctx.bool_val(true);
        bounded_time_range_constraints = bounded_time_range_constraints && (bounded_start_time <= bounded_end_time);
        bounded_time_range_constraints = bounded_time_range_constraints && (bounded_start_time >= ctx.int_val(0));
        bounded_time_range_constraints = bounded_time_range_constraints && (bounded_end_time < ctx.int_val(wl.get_total_time()));

        bounded_time_range_constraints = bounded_time_range_constraints && (bounded_start_time >= ctx.int_val(current_start_time));
        bounded_time_range_constraints = bounded_time_range_constraints && (bounded_end_time <= ctx.int_val(current_end_time));
        opt->add(bounded_time_range_constraints);

        expr bounded_current_spec = ctx.bool_val(true);
        for(int i = 0; i < wl.get_total_time(); i++){
            expr i_expr = implies(bounded_start_time <= i && i <= bounded_end_time, cp->get_expr(comp, i));
            bounded_current_spec = bounded_current_spec && i_expr;
        }

        expr bounded_not_X_or_query = !(net_model && base_wl && workload_minus_current_spec && bounded_current_spec) || query_expr;
        expr P_y = forall(all_vars, bounded_not_X_or_query);
        expr f = bounded_end_time - bounded_start_time;
        expr_vector opt_exprs(ctx);
        opt_exprs.push_back(bounded_start_time);
        opt_exprs.push_back(bounded_end_time);
        expr optimization = forall(opt_exprs, implies(P_y, f >= end_time - start_time));
        opt->add(optimization);



//        auto objective = opt->minimize(end_time - start_time);

        check_result z3_result = opt->check();

        if(z3_result == check_result::sat){
            model m = opt->get_model();
            int new_start_time = m.eval(start_time).get_numeral_int();
            int new_end_time = m.eval(end_time).get_numeral_int();
            time_range_t new_time_range = time_range_t(new_start_time, new_end_time);
            TimedSpec spec_to_try = TimedSpec(wl_spec, new_time_range, wl.get_total_time());
            // Create temp workload
            Workload temp_wl = wl;
            temp_wl.rm_spec(spec);
            temp_wl.add_spec(spec_to_try);
            if (search.check(temp_wl)) {
                // It's valid, replace the spec
                wl.rm_spec(spec);
                wl.add_spec(spec_to_try);
                continue;
            } else {
                throw std::runtime_error("Z3 returned an invalid Workload");
            }
        }else{
            throw std::runtime_error("Z3 could not find a solution");
        }

        delete opt;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    cout << "restrict_time_ranges_z3 took " << duration.count() << " milliseconds" << endl;

    return wl;
}

Workload remove_cenq_in_time_range(Workload wl, time_range_t time_range, unsigned int q) {
    // Avoid having to duplicate the code below:
    set<TimedSpec> all_specs = wl.get_all_specs();
    for (const TimedSpec& spec : all_specs) {
        wl_spec_t wl_spec = spec.get_wl_spec();
        if (holds_alternative<Comp>(wl_spec)) {
            Comp comp = get<Comp>(wl_spec);
            if (holds_alternative<Indiv>(comp.lhs)) {
                Indiv indiv = get<Indiv>(comp.lhs);
                if (indiv.queue == q) {
                    time_range_t spec_time_range = spec.get_time_range();
                    if (indiv.get_metric() == metric_t::CENQ) {
                        if (spec_time_range.first >= time_range.first &&
                            spec_time_range.second <= time_range.second) {
                            wl.rm_spec(spec);
                        }
                    }
                }
            }
        }
    }

    return wl;
}

// TODO: Perform some kind of BFS to account for mixed results
// I.e. some aipg's should be left as is, some should be transformed. Currently, we only handle the
// two extreme cases (all aipg's should be transformed, or none should be transformed)
vector<Workload> transform_aipg_to_cenq(Workload wl, Search& search) {

    // If we see a spec of the form [t1, t2]: aipg(q, t) = 1, we can replace it with [t1-1, t2]:
    // cenq(q, t) = t or [t1-1, t2]: cenq(q, t) = 1 We return a vector of the original workload and
    // the two new workloads

    before_cost["transform_aipg_to_cenq"].push_back(search.cost(wl, "transform_aipg_to_cenq"));
    cout << "Transforming aipg to cenq" << endl;

    // Queue for BFS
    std::queue<Workload> workloadQueue;
    workloadQueue.push(wl);

    set<TimedSpec> specs = wl.get_all_specs();
    for (const TimedSpec& spec : specs) {
        wl_spec_t wl_spec = spec.get_wl_spec();
        time_range_t time_range = spec.get_time_range();

        if (holds_alternative<Comp>(wl_spec)) {
            Comp comp = get<Comp>(wl_spec);
            if (holds_alternative<Indiv>(comp.lhs)) {
                Indiv indiv = get<Indiv>(comp.lhs);
                unsigned int q = indiv.queue;
                if (indiv.get_metric() == metric_t::AIPG) {
                    if (holds_alternative<unsigned int>(comp.rhs) &&
                        get<unsigned int>(comp.rhs) == 1) {
                        if (comp.get_op() ==
                            op_t::EQ) { // They should all be EQ at this point, because this
                                        // function is called before any refinement / broadening
                                        // operations have been performed

                            // At this point, we've determined that the spec we're considering (from
                            // the original workload) is of the correct aipg type We'll try to
                            // change it for each of the current Workloads in the queue

                            int queueSize = workloadQueue.size();
                            for (int i = 0; i < queueSize; i++) {
                                Workload currentWl = workloadQueue.front();
                                workloadQueue.pop();
                                workloadQueue.push(
                                    currentWl); // Push the current workload with no changes
                                                // (keeping aipg might be the better option)

                                time_range_t new_time_range = time_range_t(time_range.first - 1,
                                                                           time_range.second);

                                // First try to replace it with [t1-1, t2]: cenq(q, t) = 1
                                unsigned int one = 1;
                                TimedSpec spec_to_try = TimedSpec(Comp(Indiv(metric_t::CENQ, q),
                                                                       op_t::EQ,
                                                                       one),
                                                                  new_time_range,
                                                                  wl.get_total_time());
                                // Create temp workload
                                Workload temp_wl = currentWl;
                                temp_wl.rm_spec(
                                    spec); // TODO: Must also remove all cenq specs which
                                           // are overlapping with the aipg... Remove all
                                           // cenq specs which are overlapping with the aipg
                                temp_wl = remove_cenq_in_time_range(temp_wl, new_time_range, q);
                                temp_wl.add_spec(spec_to_try);
                                if (search.check(temp_wl, "transform_aipg_to_cenq")) {
                                    // Add to queue
                                    workloadQueue.push(temp_wl);
                                    opt_count["transform_aipg_to_cenq"]++;
                                } else {
                                    // It's not valid, try another option

                                    // Try to replace it with [t1-1, t2]: cenq(q, t) = t
                                    spec_to_try = TimedSpec(Comp(Indiv(metric_t::CENQ, q),
                                                                 op_t::EQ,
                                                                 Time(1)),
                                                            new_time_range,
                                                            wl.get_total_time());
                                    // Create temp workload
                                    temp_wl = currentWl;
                                    temp_wl.rm_spec(spec);
                                    // Remove all cenq specs which are overlapping with the aipg
                                    temp_wl = remove_cenq_in_time_range(temp_wl, new_time_range, q);
                                    temp_wl.add_spec(spec_to_try);
                                    if (search.check(temp_wl, "transform_aipg_to_cenq")) {
                                        // Add to queue
                                        workloadQueue.push(temp_wl);
                                        opt_count["transform_aipg_to_cenq"]++;
                                    } else {
                                        // It's not valid, try another option

                                        // Try to replace it with [t1-1, t2]: cenq(q, t) = t2
                                        unsigned int t2 = time_range.second;
                                        spec_to_try = TimedSpec(Comp(Indiv(metric_t::CENQ, q),
                                                                     op_t::EQ,
                                                                     t2),
                                                                new_time_range,
                                                                wl.get_total_time());
                                        // Create temp workload
                                        temp_wl = currentWl;
                                        temp_wl.rm_spec(spec);
                                        // Remove all cenq specs which are overlapping with the aipg
                                        temp_wl = remove_cenq_in_time_range(temp_wl,
                                                                            new_time_range,
                                                                            q);
                                        temp_wl.add_spec(spec_to_try);

                                        if (search.check(temp_wl, "transform_aipg_to_cenq")) {
                                            // Add to queue
                                            workloadQueue.push(temp_wl);
                                            opt_count["transform_aipg_to_cenq"]++;
                                        } else {
                                            // It's not valid, don't add any new workloads to the
                                            // queue At this point, we just keep aipg
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Return vector of all the queues left in the queue
    vector<Workload> workloads;
    while (!workloadQueue.empty()) {
        workloads.push_back(workloadQueue.front());
        workloadQueue.pop();
    }

    return workloads;
}

Workload refine(Workload wl, Search& search) {
    Workload lastValidWl = wl;
    before_cost["refine"].push_back(search.cost(wl, "refine"));

    if (!search.check(wl, "refine")) {
        throw std::runtime_error("REFINEMENT ERROR: Original workload is invalid");
    }

    lastValidWl = search.setup_refinement(lastValidWl);
    lastValidWl = search.remove_specs(lastValidWl);  // Unnecessary (should never happen, since we've already removed all unnecessary specs)
    cout << "Workload after removing specs: " << endl << lastValidWl << endl;
    lastValidWl = combine(lastValidWl, search);
    cout << "Workload after combining: " << endl << lastValidWl << endl;
    lastValidWl = search.aggregate_indivs_to_sums(lastValidWl);
    cout << "Workload after aggregating indivs to sums: " << endl << lastValidWl << endl;
    lastValidWl = broaden_operations(lastValidWl, search);
    cout << "Workload after broadening operations: " << endl << lastValidWl << endl;
    lastValidWl = search.tighten_constant_bounds(lastValidWl);
    cout << "Workload after tightening constant bounds: " << endl << lastValidWl << endl;
//    lastValidWl = restrict_time_ranges_z3(lastValidWl, search, search.cp);
//    cout << "Final Workload after restricting time ranges using Z3: " << endl << lastValidWl << endl;
    lastValidWl = restrict_time_ranges(lastValidWl, search);
    cout << "Workload after restricting time ranges: " << endl << lastValidWl << endl;

    after_cost["refine"].push_back(search.cost(lastValidWl, "refine"));
    return lastValidWl;
}

Workload improve(Workload wl, Search& search) {
    before_cost["improve"].push_back(search.cost(wl, "improve"));
    cout << "Starting improvement process" << endl;
    if (!search.check(wl, "improve")) {
        throw std::runtime_error("IMPROVEMENT ERROR: Original workload is invalid");
    }

    std::vector<Workload> possible_workloads = transform_aipg_to_cenq(wl, search);
    cout << "Done transforming aipg to cenq" << endl;
    cout << "Number of possible workloads: " << possible_workloads.size() << endl;
    if (possible_workloads.empty()) {
        throw std::runtime_error("No possible workloads generated");
    }

    std::unique_ptr<Workload> bestWorkloadPtr;
    unsigned int lowestCost = std::numeric_limits<unsigned int>::max();

    int numWorkloads = 1;

    for (auto& workload : possible_workloads) {
        cout << "Refining " << numWorkloads << "th workload" << endl;
        after_cost["transform_aipg_to_cenq"].push_back(search.cost(workload, "transform_aipg_to_cenq"));
        numWorkloads++;
        Workload refinedWorkload = refine(workload, search);
        unsigned int currentCost = search.cost(refinedWorkload, "improve");

        if (currentCost < lowestCost) {
            lowestCost = currentCost;
            bestWorkloadPtr = std::make_unique<Workload>(refinedWorkload);
        }
    }

    if (!bestWorkloadPtr) {
        throw std::runtime_error("Failed to find a best workload");
    }

    after_cost["improve"].push_back(search.cost(*bestWorkloadPtr, "improve"));
    return *bestWorkloadPtr;
}

//
// TODO:
// 0. Base example
// 1. Randomly remove
// Refinment process:
// Setup refinement
// Remove specs
// Combine (normalize) - see more examples, generalize patterns
// Aggregate indivs to sums
// Tighten constants on rhs
// Broaden operations
//
// Look at more complicated examples (already maxed out what we can do for prio) such as round-robin, and prio for more total time
//
// Separate:
// - loosen constants on rhs
// - changing indiv to sums
// - Combining (expand based on new patterns we recognize). Ask Z3 to give us the most concise with synthesis? Use get_expr to create Z3 variables for search. Ask to find an equivalent workload spec... if we want a general rhs, have mutually-exclusive booleans to represent which type of rhs we are using

Workload research_project(IndexedExample* base_eg, ContentionPoint* cp, unsigned int total_time, Query query, unsigned int max_spec, string good_examples_file, string bad_examples_file, SharedConfig* config, unsigned int good_example_cnt, unsigned int bad_example_cnt){
    // Create a workload that perfectly describes the base example
    // That is, for each queue, for each timestep, the workload has a spec of the form:
    // [t, t]: cenq(queue_id, t) = sum from 0 to t of enqs[queue_id][t]

    // Print global arguments std::vector<std::string> globalArgs;
    string searchMode = globalArgs[1];
    //    if(searchMode != "random" && searchMode != "default" && searchMode != "front_back" &&
    //    searchMode != "back_front" && searchMode != "broad"){
    //        cout << "Invalid search mode: " << searchMode << endl;
    //        return;
    //    }

    if (searchMode == "research") {
        cout << "Base example: " << endl << *base_eg << endl;

        vector<vector<unsigned int>> enqs = base_eg->enqs;
        vector<vector<unsigned int>> sums(cp->in_queue_cnt(), vector<unsigned int>(total_time, 0));
        for (unsigned int q = 0; q < enqs.size(); q++) {
            for (unsigned int t = 0; t < enqs[q].size(); t++) {
                sums[q][t] = (t == 0) ? enqs[q][t] : sums[q][t - 1] + enqs[q][t];
            }
        }

        // Add cenq specs
        Workload wl(100, cp->in_queue_cnt(), total_time);
        for (unsigned int q = 0; q < enqs.size(); q++) {
            unsigned int start_t = 0; // Start of a contiguous period
            for (unsigned int t = 1; t <= enqs[q].size(); t++) {
                // Check if we reached the end or found a change in cenq value
                if (t == enqs[q].size() || sums[q][t] != sums[q][start_t]) {
                    // We have reached the end of a contiguous period or the end of the array
                    // Add the spec for the period from start_t to t-1 with the cenq value at start_t
                    wl.add_spec(TimedSpec(Comp(Indiv(metric_t::CENQ, q), op_t::EQ, sums[q][start_t]),
                                          time_range_t(start_t, t - 1),
                                          total_time));
                    start_t = t; // Update start_t to the current time for the next period
                }
                // If the value at t is the same as at start_t, we just continue the loop
            }
        }

        // Get set of queues with no traffic (cenq at last timestep is 0)
        std::set<unsigned int> empty_queues;
        for (unsigned int q = 0; q < cp->in_queue_cnt(); q++) {
            if (sums[q][total_time - 1] == 0) {
                empty_queues.insert(q);
            }
        }

        // Add aipg specs
        Queue* q0 = cp->get_out_queue(0);


        //     Add meta-data specs (ecmp and dst)
        Ecmp ecmp = Ecmp(q0, total_time, cp->net_ctx);
        Dst dst = Dst(q0, total_time, cp->net_ctx);

        // Add ecmp and dst specs to the workload

        for (unsigned int q = 0; q < enqs.size(); q++) {
            // If empty queue, skip (in theory this is actually unnecessary, since all meta-data specs should be invalid anyway)
            if (empty_queues.find(q) != empty_queues.end()) {
                continue;
            }
            for (unsigned int t = 0; t < enqs[q].size(); t++) {
                metric_val metric_value;
                ecmp.eval(base_eg, t, q, metric_value);
                unsigned int value = metric_value.value;
                if (metric_value.valid) {
                    wl.add_spec(TimedSpec(Comp(Indiv(metric_t::ECMP, q), op_t::EQ, value),
                                          time_range_t(t, t),
                                          total_time));
                }
                dst.eval(base_eg, t, q, metric_value);
                value = metric_value.value;
                if (metric_value.valid) {
                    wl.add_spec(TimedSpec(Comp(Indiv(metric_t::DST, q), op_t::EQ, value),
                                          time_range_t(t, t),
                                          total_time));
                }
            }
        }

        // Keep randomly removing specs until we hit 'minimum' workload
        // (i.e. the workload that is valid but removing any spec makes it invalid)

        // First filter out meta-data specs (ecmp and dst) : they are not essential for some network
        // types
        auto all_specs = wl.get_all_specs();
        auto meta_data_specs = std::vector<TimedSpec>();

        Workload originalWorkload = wl;

        std::random_device rd;
        std::mt19937 gen(rd());
        wl = originalWorkload;
        Workload lastValidWl = wl; // Store the initial workload state.

        std::set<TimedSpec> attemptedSpecs; // To track specs we've attempted to remove.
        std::set<TimedSpec> essentialSpecs; // To track specs confirmed as essential.
                                            //
                                            //    // Randomly remove meta-data specs
                                            //    while (true) {
                                            //        if (meta_data_specs.empty()) {
                                            //            break;
                                            //        }
        //        std::uniform_int_distribution<> dis(0, meta_data_specs.size() - 1);
        //        auto it = std::next(meta_data_specs.begin(), dis(gen));
        //        TimedSpec const& specToRemove = *it;
        //        wl.rm_spec(specToRemove);
        //        if (search.check(wl)) {
        //            lastValidWl = wl;          // Update the last valid state.
        //            meta_data_specs.erase(it); // Remove the spec from the set of potential
        //            meta-data specs.
        //        } else {
        //            wl.add_spec(specToRemove); // Put the last removed spec back.
        //            meta_data_specs.erase(it); // Remove the spec from the set of potential
        //            meta-data specs.
        //        }
        //    }
        //
        //    while (true) {
        //        auto specs = wl.get_all_specs(); // Get the current specs.
        //
        //        // Break the loop if all current specs are confirmed as essential.
        //        if (essentialSpecs.size() == specs.size()) {
        //            break;
        //        }
        //
        //        std::uniform_int_distribution<> dis(0, specs.size() - 1);
        //        auto it = std::next(specs.begin(), dis(gen));
        //        TimedSpec const& specToRemove = *it;
        //        //        cout << "Removing spec: " << specToRemove << endl;
        //
        //        // If we've already attempted to remove this spec, skip it.
        //        if (attemptedSpecs.find(specToRemove) != attemptedSpecs.end()) {
        //            //            cout << "Spec already attempted" << endl;
        //            continue;
        //        }
        //
        //        // Attempt to remove the spec.
        //        Workload tempWl = wl;
        //        tempWl.rm_spec(specToRemove);
        //
        //        if (search.check(tempWl)) {
        //            //            cout << "Workload is valid after removing spec" << endl;
        //            wl = tempWl;            // Update the original workload if the temp one is
        //            valid. lastValidWl = wl;       // Update the last valid state.
        //            attemptedSpecs.clear(); // Reset attempted specs since the workload has
        //            changed.
        //        } else {
        //            //            cout << "Workload is invalid after removing spec" << endl;
        //            attemptedSpecs.insert(specToRemove); // Mark this spec as attempted.
        //            essentialSpecs.insert(specToRemove); // Confirm this spec as essential.
        //        }
        //    }
        //
        //    cout << "Last valid workload: " << endl << lastValidWl << endl;
        //
        //    if (!search.check(lastValidWl)) {
        //        cout << "ERROR: Last valid workload is invalid" << endl;
        //    } else {
        //        lastValidWl = search.setup_refinement(lastValidWl);
        //        lastValidWl = search.remove_specs(lastValidWl);
        //        cout << "Workload after removing specs: " << endl << lastValidWl << endl;
        //        lastValidWl = combine(lastValidWl, search);
        //        cout << "Workload after combining: " << endl << lastValidWl << endl;
        //        lastValidWl = search.aggregate_indivs_to_sums(lastValidWl);
        //        cout << "Workload after aggregating indivs to sums: " << endl << lastValidWl <<
        //        endl; lastValidWl = broaden_operations(lastValidWl, search); cout << "Workload
        //        after broadening operations: " << endl << lastValidWl << endl; lastValidWl =
        //        search.tighten_constant_bounds(lastValidWl); cout << "Workload after tightening
        //        constant bounds: " << endl << lastValidWl << endl; lastValidWl =
        //        restrict_time_ranges(lastValidWl, search); cout << "Final Workload after
        //        restricting time ranges (Random Approach): " << endl << lastValidWl << endl;
        //    }
        //
        //    Workload lastValidWlRandom = lastValidWl;
        //    unsigned int random_cost = search.cost(lastValidWlRandom);

        wl = originalWorkload;

        AIPG aipg = AIPG(q0, total_time, cp->net_ctx);
        // Add aipg specs to the workload
        for (unsigned int q = 0; q < enqs.size(); q++) {
            // If empty queue, skip (aipg conveys no information)
            if (empty_queues.find(q) != empty_queues.end()) {
                continue;
            }
            for (unsigned int t = 0; t < enqs[q].size(); t++) {
                metric_val metric_value;
                aipg.eval(base_eg, t, q, metric_value);
                unsigned int value = metric_value.value;
                // Don't add if aipg=0 (this conveys no information)
                if (metric_value.valid && value != 0) { // OPTIMIZATION: Don't add if aipg=0
                    wl.add_spec(TimedSpec(Comp(Indiv(metric_t::AIPG, q), op_t::EQ, value),
                                          time_range_t(t, t),
                                          total_time));
                }
            }
        }

        Search search(cp, query, max_spec, config, good_examples_file, bad_examples_file);
        wl = combine(wl, search);
        cout << "Original Workload: " << endl << wl << endl;
        before_cost["research"].push_back(search.cost(wl, "research"));

        if (!search.check(wl, "research")) {
            cout << "ERROR: Original workload is invalid" << endl;

            solver_res_t workload_feasible = search.cp->check_workload_without_query(wl);
            if (workload_feasible == solver_res_t::UNSAT) {
                cout << "Workload is infeasible" << endl;
            }

            IndexedExample* counter_eg = new IndexedExample();
            solver_res_t query_only_res = search.cp->check_workload_with_query(wl, counter_eg);
            if (query_only_res == solver_res_t::UNKNOWN) {
                cout << "Counter-example: " << endl << *counter_eg << endl;
            }
            throw std::runtime_error("ERROR: Original workload is invalid");
        }

        lastValidWl = wl; // Store the initial workload state.

        attemptedSpecs.clear(); // To track specs we've attempted to remove.
        essentialSpecs.clear(); // To track specs confirmed as essential.

        meta_data_specs.clear();
        for (auto const& timed_spec : all_specs) {
            wl_spec_t spec = timed_spec.get_wl_spec();
            if (holds_alternative<Comp>(spec)) {
                Comp comp = get<Comp>(spec);
                if (holds_alternative<Indiv>(comp.lhs)) {
                    Indiv indiv = get<Indiv>(comp.lhs);
                    if (indiv.get_metric() == metric_t::ECMP ||
                        indiv.get_metric() == metric_t::DST) {
                        meta_data_specs.push_back(timed_spec);
                    }
                }
            }
        }

        // Randomly remove meta-data specs
        before_cost["remove_meta_data"].push_back(search.cost(lastValidWl, "remove_meta_data"));
        cout << "Removing meta-data specs..." << endl;
        while (true) {
            if (meta_data_specs.empty()) {
                break;
            }
            std::uniform_int_distribution<> dis(0, meta_data_specs.size() - 1);
            auto it = std::next(meta_data_specs.begin(), dis(gen));
            TimedSpec const& specToRemove = *it;
            wl.rm_spec(specToRemove);
            if (search.check(wl, "remove_meta_data")) {
                lastValidWl = wl; // Update the last valid state.
                meta_data_specs.erase(
                    it); // Remove the spec from the set of potential meta-data specs.
                opt_count["remove_meta_data"]++;
            } else {
                wl.add_spec(specToRemove); // Put the last removed spec back.
                meta_data_specs.erase(
                    it); // Remove the spec from the set of potential meta-data specs.
            }
        }
        cout << "Finished removing meta-data specs" << endl;
        after_cost["remove_meta_data"].push_back(search.cost(lastValidWl, "remove_meta_data"));

        // Randomly remove specs
        before_cost["random_approach"].push_back(search.cost(lastValidWl, "random_approach"));
        cout << "Starting random approach..." << endl;
        while (true) {
            auto specs = wl.get_all_specs(); // Get the current specs.

            // Break the loop if all current specs are confirmed as essential.
            if (essentialSpecs.size() == specs.size()) {
                break;
            }

            // Sort the specs by time range length (
            std::vector<TimedSpec> sortedSpecs(specs.begin(), specs.end());
            std::sort(sortedSpecs.begin(),
                      sortedSpecs.end(),
                      [](const TimedSpec& a, const TimedSpec& b) {
                          return a.get_time_range().second - a.get_time_range().first <
                                 b.get_time_range().second - b.get_time_range().first;
                      });

            // Pick the shortest time range spec, keep picking until we find one we havent't tried
            int index = 0;
            TimedSpec specToRemove = sortedSpecs[index];
            while (true) {
                if (index == sortedSpecs.size()) {
                    break;
                }
                specToRemove = sortedSpecs[index];
                if (attemptedSpecs.find(specToRemove) != attemptedSpecs.end()) {
                    index++;
                    continue;
                }
                break;
            }

            // Attempt to remove the spec.
            Workload tempWl = wl;
            tempWl.rm_spec(specToRemove);

            if (search.check(tempWl, "random_approach")) {
                //            cout << "Workload is valid after removing spec" << endl;
                wl = tempWl;            // Update the original workload if the temp one is valid.
                lastValidWl = wl;       // Update the last valid state.
                attemptedSpecs.clear(); // Reset attempted specs since the workload has changed.
                opt_count["random_approach"]++;
            } else {
                //            cout << "Workload is invalid after removing spec" << endl;
                attemptedSpecs.insert(specToRemove); // Mark this spec as attempted.
                essentialSpecs.insert(specToRemove); // Confirm this spec as essential.
            }
        }

        cout << "Last valid workload: " << endl << lastValidWl << endl;
        after_cost["random_approach"].push_back(search.cost(lastValidWl, "random_approach"));

        if (!search.check(lastValidWl, "random_approach")) {
            throw std::runtime_error("ERROR: Last valid workload is invalid");
        } else {
            lastValidWl = improve(lastValidWl, search);
            cout << "Final Workload: " << endl << lastValidWl << endl;
            after_cost["research"].push_back(search.cost(lastValidWl, "research"));

            search.print_stats();
        }


        timeline_t wl_timeline = lastValidWl.get_timeline();
        cout << "Timeline: " << endl;
        for (auto const& [time, specs] : wl_timeline) {
            cout << "Time: " << time << endl;
            for (auto const& spec : specs) {
                cout << spec << endl;
            }
        }

        return lastValidWl;

        //    Workload lastValidWlBroad = lastValidWl;
        //    unsigned int broad_cost = search.cost(lastValidWlBroad);
        //    cout << "Random Approach Cost: " << random_cost << endl;
        //    cout << "Approach Cost: " << broad_cost << endl;

        // Try another approach: start from the original workload and remove specs one by one
        // by doing the following procedure:
        // For each queue q:
        // For t from 1 to total_time:
        // Remove the spec that specifies cenq(q, t) = sum from 0 to t of enqs[q][t] until the
        // workload becomes invalid (then put the last removed spec back and move on to the next
        // queue)

        //    if(searchMode == "front_back") {
        //        Workload wl2 = wl;
        //        for (unsigned int q = 0; q < enqs.size(); q++) {
        //            for (unsigned int t = 0; t < enqs[q].size(); t++) {
        //                TimedSpec specToRemove(Comp(Indiv(metric_t::CENQ, q), op_t::EQ,
        //                sums[q][t]),
        //                                       time_range_t(t, t),
        //                                       total_time);
        //                wl2.rm_spec(specToRemove);
        //                //            cout << "Removing spec: " << specToRemove << endl;
        //                if (!search.check(wl2)) {
        //                    //                cout << "Workload is invalid" << endl;
        //                    wl2.add_spec(specToRemove);
        //                    break;
        //                }
        //            }
        //        }
        //
        //        cout << "Last valid workload: " << endl << wl2 << endl;
        //        wl2 = search.setup_refinement(wl2);
        //        wl2 = search.remove_specs(wl2);
        //        cout << "Workload after removing specs: " << endl << wl2 << endl;
        //        wl2 = combine(wl2, search);
        //        cout << "Workload after combining: " << endl << wl2 << endl;
        //        wl2 = search.aggregate_indivs_to_sums(wl2);
        //        cout << "Workload after aggregating indivs to sums: " << endl << wl2 << endl;
        //        wl2 = search.tighten_constant_bounds(wl2);
        //        cout << "Workload after tightening constant bounds: " << endl << wl2 << endl;
        //        wl2 = broaden_operations(wl2, search);
        //        cout << "Final Workload after broadening operations (Front-to-Back Iterative
        //        Approach): "
        //             << endl
        //             << wl2 << endl;
        //    }

        // Back-to-Front approach: same as above, except we iterate on t from total_time to 1
        // WARNING: time starts at 1, can never equal 0
        //    if(searchMode == "back_front") {
        //        Workload wl3 = wl;
        //
        //        for (unsigned int q = 0; q < enqs.size(); q++) {
        //    //        cout << "Number of timesteps for queue " << q << ": " << enqs[q].size() <<
        //    endl;
        //            if (!enqs[q].empty()) {
        //                for (int t = static_cast<int>(enqs[q].size()) - 1; t >= 0; t--) {
        //                    TimedSpec specToRemove(Comp(Indiv(metric_t::CENQ, q), op_t::EQ,
        //                    sums[q][t]), time_range_t(static_cast<unsigned int>(t),
        //                    static_cast<unsigned int>(t)),
        //                            total_time);
        //                    // Check if spec is in the workload
        //                    if (wl3.get_all_specs().find(specToRemove) ==
        //                    wl3.get_all_specs().end()) {
        //    //                    cout << "Spec " << specToRemove << " not in workload" << endl;
        //                        continue;
        //                    }
        //    //                cout << "Removing spec: " << specToRemove << endl;
        //                    wl3.rm_spec(specToRemove);
        //                    if (!search.check(wl3)) {
        //    //                    cout << "Workload is invalid" << endl;
        //                        wl3.add_spec(specToRemove);
        //                        break;
        //                    } else {
        //    //                    cout << "Workload is valid" << endl;
        //                    }
        //                }
        //            }
        //        }
        //
        //
        //        cout << "Last valid workload: " << endl << wl3 << endl;
        //        wl3 = search.setup_refinement(wl3);
        //        wl3 = search.remove_specs(wl3);
        //        cout << "Workload after removing specs: " << endl << wl3 << endl;
        //        wl3 = combine(wl3, search);
        //        cout << "Workload after combining: " << endl << wl3 << endl;
        //        wl3 = search.aggregate_indivs_to_sums(wl3);
        //        cout << "Workload after aggregating indivs to sums: " << endl << wl3 << endl;
        //        wl3 = search.tighten_constant_bounds(wl3);
        //        cout << "Workload after tightening constant bounds: " << endl << wl3 << endl;
        //        wl3 = broaden_operations(wl3, search);
        //        cout << "Final Workload after broadening operations (Back-to-Front Iterative
        //        Approach): " << endl << wl3 << endl;
        //
        //    }
    } else if (searchMode == "default") {
        run(cp,
            base_eg,
            good_example_cnt,
            good_examples_file,
            bad_example_cnt,
            bad_examples_file,
            query,
            8,
            config);
    } else if (searchMode == "test"){
        Search search(cp, query, max_spec, config, good_examples_file, bad_examples_file);

        // Create Workload:
//        [1, 6]: SUM_[q in {0, 1, }] cenq(q ,t) >= t
//        [1, 7]: cenq(2, t) >= 1
        Workload wl(100, cp->in_queue_cnt(), total_time);
        wl.clear();
        time_range_t time_range1 = time_range_t(0, 6); // Optimal is [0, 5]
        time_range_t time_range2 = time_range_t(0, 6);
        qset_t queues = {0, 1};
        wl.add_spec(TimedSpec(Comp(QSum(queues, metric_t::CENQ), op_t::GE, Time(1)), time_range1, total_time));
        wl.add_spec(TimedSpec(Comp(Indiv(metric_t::CENQ, 2), op_t::GE, 1u), time_range2, total_time));
        cout << "Original Workload: " << endl << wl << endl;
        if(!search.check(wl)){
            cout << "ERROR: Original workload is invalid" << endl;
        }

        // Test restrict_time_ranges_z3
        wl = restrict_time_ranges_z3(wl, search, cp);
        cout << "Workload after restrict_time_ranges_z3: " << endl << wl << endl;
    } else if (searchMode == "z3_test"){
        std::cout << "quantifier example\n";
        context c;

        expr x = c.int_const("x");
        expr y = c.int_const("y");
        z3::sort I = c.int_sort();
        func_decl f = z3::function("f", I, I, I);

        solver s(c);

        // making sure model based quantifier instantiation is enabled.
        params p(c);
        p.set("mbqi", true);
        s.set(p);

        expr_vector vars(c);
        vars.push_back(x);
        vars.push_back(y);
        s.add(forall(vars, f(x, y) >= 0));
        expr a = c.int_const("a");
        s.add(f(a, a) < a);
        std::cout << s << "\n";
        std::cout << s.check() << "\n";
        std::cout << s.get_model() << "\n";
        s.add(a < 0);
        std::cout << s.check() << "\n";
    } else if (searchMode == "z3_test2"){
//        z3::context c;
//        z3::solver s(c);
//
//        // Example usage
//        z3::expr x = c.bool_const("x");
//        z3::expr y = c.bool_const("y");
//        s.add(x && !y);
//        s.add(!x || y);
//
//        auto vars = get_all_vars(s);
//        for (auto& var : vars) {
//            std::cout << var << std::endl;
//        }
    } else if (searchMode == "z3_test3"){
//        auto& ctx = cp->net_ctx.z3_ctx();
//
//        z3::solver* s = cp->z3_solver;
//
//        auto vars = get_all_vars(*s);
//        vector<expr> error_free_vars;
//
//        // TEST
//        // For each variable:
//        // Create an expr_vector containing only that variable
//        // Create a forall quantifier with that variable
//        // If it causes an error, print the variable
//        for(auto& var : vars){
//            z3::expr_vector expression_vector(ctx);
//            expression_vector.push_back(var);
//            try {
//                z3::expr forall = z3::forall(expression_vector, ctx.bool_val(true));
//                // Add it to list of error-free variables
//
//                if(var.is_const()){
//                    error_free_vars.push_back(var);
//                }
//
//            } catch (z3::exception e){
//                std::cout << "Error with variable: " << var << std::endl;
//            }
//        }
//
//        cout << "Original number of variables: " << vars.size() << endl;
//        cout << "Number of errors: " << vars.size() - error_free_vars.size() << endl;
//        cout << "Number of error-free variables: " << error_free_vars.size() << endl;

    } else if (searchMode == "z3_test4"){
        expr_vector* all_vars = cp->net_ctx.list_of_vars;

        // Print each variable
        for (unsigned int i = 0; i < all_vars->size(); i++) {
            std::cout << (*all_vars)[i] << std::endl;
        }

        // Print number of bool and int variables
        unsigned int bool_vars = 0;
        unsigned int int_vars = 0;
        for (unsigned int i = 0; i < all_vars->size(); i++) {
            if ((*all_vars)[i].is_bool()) {
                bool_vars++;
            } else if ((*all_vars)[i].is_int()) {
                int_vars++;
            }
        }
        std::cout << "Number of bool variables: " << bool_vars << std::endl;
        std::cout << "Number of int variables: " << int_vars << std::endl;

        // Try creating a forall quantifier with each variable
        for (unsigned int i = 0; i < all_vars->size(); i++) {
            z3::expr_vector expression_vector(cp->net_ctx.z3_ctx());
            expression_vector.push_back((*all_vars)[i]);
            try {
                z3::expr forall = z3::forall(expression_vector, cp->net_ctx.z3_ctx().bool_val(true));
            } catch (z3::exception e) {
                std::cout << "Error with variable: " << (*all_vars)[i] << std::endl;
            }
        }
    } else if (searchMode == "cost_test") {

        // Create workload:
        // [1, 2]: cenq(0, t) = 3


        Workload wl(100, cp->in_queue_cnt(), total_time);
        wl.clear();
        wl.add_spec(TimedSpec(Comp(Indiv(metric_t::CENQ, 0), op_t::EQ, 3u), time_range_t(0, 1), total_time));

        Search search(cp, query, max_spec, config, good_examples_file, bad_examples_file);
        cout << "Original Workload: " << endl << wl << endl;
        cout << "Num traces: " << search.indiv_equal_to_num_traces(wl) << endl;


        return wl;
    } else {
        throw std::runtime_error("Invalid search mode");
    }

}

void prio(string good_examples_file, string bad_examples_file) {

    cout << "Prio" << endl;
    time_typ start_time = noww();

    unsigned int prio_levels = 4;
    unsigned int query_thresh = 5;

    unsigned int good_example_cnt = 50;
    unsigned int bad_example_cnt = 50;
    unsigned int total_time = 7;

    PrioScheduler* prio = new PrioScheduler(prio_levels, total_time);

    cid_t query_qid = prio->get_in_queues()[2]->get_id();
    Query query(query_quant_t::EXISTS,
                time_range_t(0, prio->get_total_time() - 1),
                query_qid,
                metric_t::CBLOCKED,
                op_t::GT,
                query_thresh);

    prio->set_query(query);

    cout << "cp setup: " << (get_diff_millisec(start_time, noww()) / 1000.0) << " s" << endl;

    // generate base example
    start_time = noww();
    IndexedExample* base_eg = new IndexedExample();
    qset_t target_queues;

    bool res = prio->generate_base_example(base_eg, target_queues, prio_levels);

    if (!res) {
        cout << "ERROR: couldn't generate base example" << endl;
        return;
    }

    cout << "base example generation: " << (get_diff_millisec(start_time, noww()) / 1000.0) << " s"
         << endl;

    // Set shared config
    DistsParams dists_params;
    dists_params.in_queue_cnt = prio->in_queue_cnt();
    dists_params.total_time = total_time;
    dists_params.pkt_meta1_val_max = 2;
    dists_params.pkt_meta2_val_max = 2;
    dists_params.random_seed = 2000;

    Dists* dists = new Dists(dists_params);
    SharedConfig* config = new SharedConfig(total_time, prio->in_queue_cnt(), target_queues, dists);
    bool config_set = prio->set_shared_config(config);
    if (!config_set) return;


    research_project(base_eg, prio, total_time, query, 8, good_examples_file, bad_examples_file, config, good_example_cnt, bad_example_cnt);
}

void rr(string good_examples_file, string bad_examples_file) {

    vector<Workload> workloads;

    unsigned int loop_count = 0;

    while(true) {
        loop_count++;
        if(loop_count >= 5) {
            throw std::runtime_error("Loop count exceeded");
        }
        cout << "rr" << endl;
        time_typ start_time = noww();

        unsigned int in_queue_cnt = 5;
        unsigned int period = 5;
        unsigned int recur = 2;
        unsigned int rate = 4;

        unsigned int good_example_cnt = 25;
        unsigned int bad_example_cnt = 25;
        unsigned int total_time = recur * period;

        // Create contention point
        RRScheduler* rr = new RRScheduler(in_queue_cnt, total_time);

        unsigned int queue1 = 1;
        unsigned int queue2 = 2;

        // Base workload
        Workload wl(100, in_queue_cnt, total_time);

        for (unsigned int i = 1; i <= recur; i++) {
            for (unsigned int q = 0; q < in_queue_cnt; q++) {
                wl.add_spec(TimedSpec(Comp(Indiv(metric_t::CENQ, q), op_t::GE, i * rate),
                                      time_range_t(i * period - 1, i * period - 1),
                                      total_time));
            }
        }

        wl.add_spec(
            TimedSpec(Comp(Indiv(metric_t::CENQ, queue1), op_t::GT, Indiv(metric_t::CENQ, queue2)),
                      time_range_t(total_time - 1, total_time - 1),
                      total_time));

        cout << "base workload: " << endl << wl << endl;

        for(const Workload& workload : workloads) {
            rr->add_past_workload(workload);
        }
        rr->set_base_workload(wl);

        // Query
        cid_t queue1_id = rr->get_in_queues()[queue1]->get_id();
        cid_t queue2_id = rr->get_in_queues()[queue2]->get_id();

        Query query(query_quant_t::FORALL,
                    time_range_t(total_time - 1 - (period - 1), total_time - 1),
                    qdiff_t(queue2_id, queue1_id),
                    metric_t::CDEQ,
                    op_t::GE,
                    3);

        rr->set_query(query);

        cout << "cp setup: " << (get_diff_millisec(start_time, noww()) / 1000.0) << " s" << endl;

        // generate base example
        start_time = noww();
        IndexedExample* base_eg = new IndexedExample();
        qset_t target_queues;

        bool res = rr->generate_base_example(base_eg, target_queues, in_queue_cnt);

        if (!res) {
            cout << "ERROR: couldn't generate base example" << endl;
            return;
        }

        cout << "base example generation: " << (get_diff_millisec(start_time, noww()) / 1000.0) << " s"
             << endl;


        // Set shared config
        DistsParams dists_params;
        dists_params.in_queue_cnt = rr->in_queue_cnt();
        dists_params.total_time = total_time;
        dists_params.pkt_meta1_val_max = 2;
        dists_params.pkt_meta2_val_max = 2;
        dists_params.random_seed = 29663;

        Dists* dists = new Dists(dists_params);
        SharedConfig* config = new SharedConfig(total_time, rr->in_queue_cnt(), target_queues, dists);
        bool config_set = rr->set_shared_config(config);
        if (!config_set) return;

        workloads.push_back(research_project(base_eg, rr, total_time, query, 10, good_examples_file, bad_examples_file, config, good_example_cnt, bad_example_cnt));
    }
}

void fq_codel(string good_examples_file, string bad_examples_file) {

    vector<Workload> workloads;

    unsigned int loop_count = 0;

    while(true) {

         if(loop_count >= 10) {
            throw std::runtime_error("Loop count exceeded");
        }

        cout << "fq_codel" << endl;
        time_typ start_time = noww();

        unsigned int in_queue_cnt = 5;
        unsigned int total_time = 14;
        unsigned int query_thresh = (total_time / in_queue_cnt) + 3;
        unsigned int last_queue = in_queue_cnt - 1;

        unsigned int good_example_cnt = 50;
        unsigned int bad_example_cnt = 50;

        // Create contention point
        Buggy2LRRScheduler* cp = new Buggy2LRRScheduler(in_queue_cnt, total_time);

        // Base Workload
        Workload wl(in_queue_cnt * 5, in_queue_cnt, total_time);
        for (unsigned int q = 0; q < last_queue; q++) {
            wl.add_spec(
                TimedSpec(Comp(Indiv(metric_t::CENQ, q), op_t::GE, Time(1)), total_time, total_time));
        }

        for(Workload workload : workloads) {
            cp->add_past_workload(workload);
        }
        cp->set_base_workload(wl);

        // Query
        cid_t query_qid = cp->get_in_queues()[last_queue]->get_id();
        Query query(query_quant_t::FORALL,
                    time_range_t(total_time - 1, total_time - 1),
                    query_qid,
                    metric_t::CDEQ,
                    op_t::GE,
                    query_thresh);

        cp->set_query(query);

        cout << "cp setup: " << (get_diff_millisec(start_time, noww()) / 1000.0) << " s" << endl;

        // generate base example
        start_time = noww();
        IndexedExample* base_eg = new IndexedExample();
        qset_t target_queues;

        bool res = cp->generate_base_example(base_eg, target_queues, in_queue_cnt);

        if (!res) {
            cout << "ERROR: couldn't generate base example" << endl;
            return;
        }

        cout << "base example generation: " << (get_diff_millisec(start_time, noww()) / 1000.0) << " s"
             << endl;


        // Set shared config
        DistsParams dists_params;
        dists_params.in_queue_cnt = cp->in_queue_cnt();
        dists_params.total_time = total_time;
        dists_params.pkt_meta1_val_max = 2;
        dists_params.pkt_meta2_val_max = 2;
        dists_params.random_seed = 4854;

        Dists* dists = new Dists(dists_params);
        SharedConfig* config = new SharedConfig(total_time, cp->in_queue_cnt(), target_queues, dists);
        bool config_set = cp->set_shared_config(config);
        if (!config_set) return;

        workloads.push_back(research_project(base_eg, cp, total_time, query, 10, good_examples_file, bad_examples_file, config, good_example_cnt, bad_example_cnt));
    }
}

void loom(string good_examples_file, string bad_examples_file) {

    vector<Workload> workloads;

    while(true) {
        cout << "loom" << endl;
        time_typ start_time = noww();

        unsigned int nic_tx_queue_cnt = 4;
        unsigned int per_core_flow_cnt = 3;
        unsigned int query_time = 3;

        unsigned int good_example_cnt = 50;
        unsigned int bad_example_cnt = 50;
        unsigned int total_time = 10;

        // Create contention point
        LoomMQPrio* cp = new LoomMQPrio(nic_tx_queue_cnt, per_core_flow_cnt, total_time);


        qset_t tenant1_qset;
        qset_t tenant2_qset;

        for (unsigned int i = 0; i < cp->in_queue_cnt(); i++) {
            if (i % 3 == 0)
                tenant1_qset.insert(i);
            else
                tenant2_qset.insert(i);
        }

        // Base Workload

        Workload wl(20, cp->in_queue_cnt(), total_time);
        wl.add_spec(TimedSpec(Comp(QSum(tenant1_qset, metric_t::CENQ), op_t::GE, Time(1)),
                              total_time,
                              total_time));
        wl.add_spec(TimedSpec(Comp(QSum(tenant2_qset, metric_t::CENQ), op_t::GE, Time(1)),
                              total_time,
                              total_time));

        for (unsigned int q = 0; q < cp->in_queue_cnt(); q++) {
            if (q % 3 == 2) {
                wl.add_spec(
                    TimedSpec(Comp(Indiv(metric_t::CENQ, q), op_t::LE, 0u), total_time, total_time));
            }
        }

        for(Workload workload : workloads) {
            cp->add_past_workload(workload);
        }
        cp->set_base_workload(wl);

        // Query
        Query query(query_quant_t::FORALL,
                    time_range_t(total_time - 1 - query_time, total_time - 1),
                    qdiff_t(cp->get_out_queue(1)->get_id(), cp->get_out_queue(0)->get_id()),
                    metric_t::CENQ,
                    op_t::GT,
                    3u);

        cp->set_query(query);

        cout << "cp setup: " << (get_diff_millisec(start_time, noww()) / 1000.0) << " s" << endl;

        // generate base example
        start_time = noww();
        IndexedExample* base_eg = new IndexedExample();
        qset_t target_queues;

        bool res = cp->generate_base_example(base_eg, target_queues, cp->in_queue_cnt());

        if (!res) {
            cout << "ERROR: couldn't generate base example" << endl;
            return;
        }

        cout << "base example generation: " << (get_diff_millisec(start_time, noww()) / 1000.0) << " s"
             << endl;


        // Set shared config
        DistsParams dists_params;
        dists_params.in_queue_cnt = cp->in_queue_cnt();
        dists_params.total_time = total_time;
        dists_params.pkt_meta1_val_max = 3;
        dists_params.pkt_meta2_val_max = 2;
        dists_params.random_seed = 13388;

        Dists* dists = new Dists(dists_params);
        SharedConfig* config = new SharedConfig(total_time, cp->in_queue_cnt(), target_queues, dists);
        bool config_set = cp->set_shared_config(config);
        if (!config_set) return;

        workloads.push_back(research_project(base_eg, cp, total_time, query, 24, good_examples_file, bad_examples_file, config, good_example_cnt, bad_example_cnt));
    }

}

void leaf_spine_bw(string good_examples_file, string bad_examples_file) {

    vector<Workload> workloads;

    while(true) {
        cout << "leaf_spine_bw" << endl;
        time_typ start_time = noww();

        unsigned int leaf_cnt = 3;
        unsigned int spine_cnt = 2;
        unsigned int servers_per_leaf = 2;
        unsigned int server_cnt = leaf_cnt * servers_per_leaf;
        bool reduce_queues = true;

        unsigned int src_server = 0;
        unsigned int dst_server = (2 * servers_per_leaf) + 1;
        unsigned int query_thresh = 4;

        unsigned int good_example_cnt = 50;
        unsigned int bad_example_cnt = 50;
        unsigned int total_time = 10;

        // Create contention point
        LeafSpine* cp = new LeafSpine(leaf_cnt, spine_cnt, servers_per_leaf, total_time, reduce_queues);

        unsigned int in_queue_cnt = cp->in_queue_cnt();

        // Base Workload
        Workload wl(in_queue_cnt + 5, in_queue_cnt, total_time);

        wl.add_spec(TimedSpec(Comp(Indiv(metric_t::CENQ, src_server), op_t::GE, Time(1)),
                              total_time - 1,
                              total_time));

        wl.add_spec(TimedSpec(Comp(Indiv(metric_t::DST, src_server), op_t::EQ, dst_server),
                              total_time - 1,
                              total_time));

        for (unsigned int q = 0; q < in_queue_cnt; q++) {
            Same s(metric_t::DST, q);
            wl.add_spec(TimedSpec(s, time_range_t(0, total_time - 1), total_time));
        }

        qset_t unique_qset;
        for (unsigned int q = 0; q < in_queue_cnt; q++)
            unique_qset.insert(q);
        Unique uniq(metric_t::DST, unique_qset);
        wl.add_spec(TimedSpec(uniq, time_range_t(0, total_time - 1), total_time));

        for(Workload workload : workloads) {
            cp->add_past_workload(workload);
        }
        cp->set_base_workload(wl);

        // Query
        cid_t query_qid = cp->get_out_queue(dst_server)->get_id();
        Query query(query_quant_t::FORALL,
                    time_range_t(total_time - 1, total_time - 1),
                    query_qid,
                    metric_t::CENQ,
                    op_t::LE,
                    query_thresh);

        cp->set_query(query);

        cout << "cp setup: " << (get_diff_millisec(start_time, noww()) / 1000.0) << " s" << endl;

        // generate base example
        start_time = noww();
        IndexedExample* base_eg = new IndexedExample();
        qset_t target_queues;

        bool res = cp->generate_base_example(base_eg, target_queues, cp->in_queue_cnt());

        if (!res) {
            cout << "ERROR: couldn't generate base example" << endl;
            return;
        }

        cout << "base example generation: " << (get_diff_millisec(start_time, noww()) / 1000.0) << " s"
             << endl;


        // Set shared config
        DistsParams dists_params;
        dists_params.in_queue_cnt = cp->in_queue_cnt();
        dists_params.total_time = total_time;
        dists_params.pkt_meta1_val_max = server_cnt - 1;
        dists_params.pkt_meta2_val_max = spine_cnt - 1;
        dists_params.random_seed = 24212;

        Dists* dists = new Dists(dists_params);
        SharedConfig* config = new SharedConfig(total_time, cp->in_queue_cnt(), target_queues, dists);
        bool config_set = cp->set_shared_config(config);
        if (!config_set) return;

        workloads.push_back(research_project(base_eg, cp, total_time, query, 24, good_examples_file, bad_examples_file, config, good_example_cnt, bad_example_cnt));
    }
}

void tbf(std::string good_examples_file, std::string bad_examples_file) {

    vector<Workload> workloads;

    while(true) {
        unsigned int total_time = 6;
        unsigned int start = 2;
        unsigned int interval = 2;

        unsigned int link_rate = 3;

        TBFInfo info;
        info.link_rate = link_rate;
        info.max_tokens = 6;
        info.max_enq = 10;

        TBF* tbf = new TBF(total_time, info);

        Workload wl(100, 1, total_time);

        for (uint i = 0; i < interval; i++) {
            wl.add_spec(
                TimedSpec(Comp(Indiv(metric_t::CENQ, 0), op_t::GE, (unsigned int) (i + 1) * link_rate),
                          time_range_t(start + i, start + i),
                          total_time));
        }

        for(Workload workload : workloads) {
            tbf->add_past_workload(workload);
        }
        tbf->set_base_workload(wl);

        cid_t queue_id = tbf->get_in_queue()->get_id();

        Query query(query_quant_t::EXISTS,
                    time_range_t(0, total_time - 1),
                    queue_id,
                    metric_t::DEQ,
                    op_t::GT,
                    link_rate);

        tbf->set_query(query);

        IndexedExample* base_eg = new IndexedExample();
        qset_t target_queues;

        bool res = tbf->generate_base_example(base_eg, target_queues, 1);

        if (!res) {
            cout << "ERROR: couldn't generate base example" << endl;
            return;
        }

        // Set shared config
        DistsParams dists_params;
        dists_params.in_queue_cnt = tbf->in_queue_cnt();
        dists_params.total_time = total_time;
        dists_params.pkt_meta1_val_max = 2;
        dists_params.pkt_meta2_val_max = 2;
        dists_params.random_seed = 14748;

        Dists* dists = new Dists(dists_params);
        SharedConfig* config = new SharedConfig(total_time, tbf->in_queue_cnt(), target_queues, dists);
        bool config_set = tbf->set_shared_config(config);
        if (!config_set) return;

        workloads.push_back(research_project(base_eg, tbf, total_time, query, 8, good_examples_file, bad_examples_file, config, 50, 50));
    }

}

void run(ContentionPoint* cp,
         IndexedExample* base_eg,
         unsigned int good_example_cnt,
         string good_examples_file,
         unsigned int bad_example_cnt,
         string bad_examples_file,
         Query& query,
         unsigned int max_spec,
         SharedConfig* config) {

    // generate good examples
    time_typ start_time = noww();

    deque<IndexedExample*> good_examples;

    cp->generate_good_examples2(base_eg, good_example_cnt, good_examples);

    cout << "good example generation: " << (get_diff_millisec(start_time, noww()) / 1000.0) << " s"
         << endl;

    // write the good examples to a file
    deque<Example*> good_unindexed_examples;
    if (!good_examples_file.empty()) {
        for (deque<IndexedExample*>::iterator it = good_examples.begin(); it != good_examples.end();
             it++) {
            Example* eg = cp->unindex_example(*it);
            good_unindexed_examples.push_back(eg);
        }

        write_examples_to_file(good_unindexed_examples, good_examples_file);
    }

    // generate bad examples
    start_time = noww();
    deque<IndexedExample*> bad_examples;

    cp->generate_bad_examples(bad_example_cnt, bad_examples);

    cout << "bad example generation: " << (get_diff_millisec(start_time, noww()) / 1000.0) << " s"
         << endl;

    // write the bad examples to a file
    deque<Example*> bad_unindexed_examples;
    if (!bad_examples_file.empty()) {
        for (deque<IndexedExample*>::iterator it = bad_examples.begin(); it != bad_examples.end();
             it++) {
            Example* eg = cp->unindex_example(*it);
            good_unindexed_examples.push_back(eg);
        }

        write_examples_to_file(bad_unindexed_examples, bad_examples_file);
    }

    start_time = noww();
    // search
    Search search(cp, query, max_spec, config, good_examples, bad_examples);

    cout << "search setup: " << (get_diff_millisec(start_time, noww())) << " ms" << endl;
    search.run();
}
