/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    optsmt.cpp

Abstract:
   
    Objective optimization method.

Author:

    Anh-Dung Phan (t-anphan) 2013-10-16

Notes:


    Suppose we obtain solution t1 = k1, ..., tn = kn-epsilon
    Assert:
      t1 > k1 \/ t2 > k2 \/ ... \/ tn >= kn
    If this solution is satisfiable, then for each t_i, maximize the 
    assignment and assert the new frontier.
    Claim: we don't necessarily have to freeze assignments of 
    t_i when optimizing assignment for t_j
    because the state will always satisfy the disjunction.
    If one of the k_i is unbounded, then omit a disjunction for it.
    Claim: the end result (when the constraints are no longer feasible) 
    is Pareto optimal, but convergence will probably not be as fast
    as when fixing one parameter at a time.
    E.g., a different approach is first to find a global maximal for one
    variable. Then add a method to "freeze" that variable at the extremum if it is finite.
    To do this, add lower and upper bounds for that variable using infinitesimals.
    If the variable is unbounded, then this is of course not sufficient by itself.
        
    

--*/


#include "optsmt.h"
#include "opt_solver.h"
#include "arith_decl_plugin.h"
#include "theory_arith.h"
#include "ast_pp.h"
#include "model_pp.h"
#include "th_rewriter.h"
#include "opt_params.hpp"

namespace opt {


    void optsmt::set_cancel(bool f) {
        m_cancel = true;
    }

    void optsmt::set_max(vector<inf_eps>& dst, vector<inf_eps> const& src) {
        for (unsigned i = 0; i < src.size(); ++i) {
            if (src[i] > dst[i]) {
                dst[i] = src[i];
            }
        }
    }

    /*
        Enumerate locally optimal assignments until fixedpoint.
    */
    lbool optsmt::basic_opt() {
        opt_solver::toggle_objective _t(*m_s, true);          
        lbool is_sat = l_true;

        while (is_sat == l_true && !m_cancel) {
            is_sat = m_s->check_sat(0, 0); 
            if (is_sat == l_true) {
                update_lower();
            }
        }      
        
        if (m_cancel || is_sat == l_undef) {
            return l_undef;
        }

        // set the solution tight.
        for (unsigned i = 0; i < m_lower.size(); ++i) {
            m_upper[i] = m_lower[i];
        }
        
        return l_true;        
    }

    /*
        Enumerate locally optimal assignments until fixedpoint.
    */
    lbool optsmt::farkas_opt() {
        smt::theory_opt& opt = m_s->get_optimizer();

        if (typeid(smt::theory_inf_arith) != typeid(opt)) {
            return l_undef;
        }

        opt_solver::toggle_objective _t(*m_s, true);
        lbool is_sat= l_true;

        while (is_sat == l_true && !m_cancel) {
            is_sat = update_upper();
        }      
        
        if (m_cancel || is_sat == l_undef) {
            return l_undef;
        }

        // set the solution tight.
        for (unsigned i = 0; i < m_lower.size(); ++i) {
            m_upper[i] = m_lower[i];
        }

        return l_true;        
    }

    void optsmt::update_lower(unsigned idx, rational const& r) {
        inf_eps v(r);
        if (m_lower[idx] < v) {
            m_lower[idx] = v;
            if (m_s) m_s->get_model(m_model);
        }
    }

    void optsmt::update_lower() {
        model_ref md;
        m_s->get_model(md);
        set_max(m_lower, m_s->get_objective_values());
        IF_VERBOSE(1, 
                   for (unsigned i = 0; i < m_lower.size(); ++i) {
                       verbose_stream() << m_lower[i] << " ";
                   }
                   verbose_stream() << "\n";
                   model_pp(verbose_stream(), *md);
                   );
        expr_ref_vector disj(m);
        expr_ref constraint(m);
        
        for (unsigned i = 0; i < m_lower.size(); ++i) {
            inf_eps const& v = m_lower[i];
            disj.push_back(m_s->mk_gt(i, v));
        }
        constraint = m.mk_or(disj.size(), disj.c_ptr());
        m_s->assert_expr(constraint);
    }

    lbool optsmt::update_upper() {
        smt::theory_opt& opt = m_s->get_optimizer();
        SASSERT(typeid(smt::theory_inf_arith) == typeid(opt));
        smt::theory_inf_arith& th = dynamic_cast<smt::theory_inf_arith&>(opt); 

        expr_ref bound(m);
        expr_ref_vector bounds(m);

        solver::scoped_push _push(*m_s);

        //
        // NB: we have to create all bound expressions before calling check_sat
        // because the state after check_sat is not at base level.
        //

        vector<inf_eps> mid;

        for (unsigned i = 0; i < m_lower.size() && !m_cancel; ++i) {
            if (m_lower[i] < m_upper[i]) {
                smt::theory_var v = m_vars[i]; 
                mid.push_back((m_upper[i]+m_lower[i])/rational(2));
                //mid.push_back(m_upper[i]);
                bound = th.mk_ge(v, mid[i]);
                bounds.push_back(bound);
            }
            else {
                bounds.push_back(0);
                mid.push_back(inf_eps());
            }
        }
        bool progress = false;
        for (unsigned i = 0; i < m_lower.size() && !m_cancel; ++i) {
            if (m_lower[i] <= mid[i] && mid[i] <= m_upper[i] && m_lower[i] < m_upper[i]) {
                th.enable_record_conflict(bounds[i].get());
                lbool is_sat = m_s->check_sat(1, bounds.c_ptr() + i);
                switch(is_sat) {
                case l_true:
                    IF_VERBOSE(2, verbose_stream() << "Setting lower bound for v" << m_vars[i] << " to " << m_upper[i] << "\n";);
                    m_lower[i] = mid[i];
                    th.enable_record_conflict(0);
                    update_lower();
                    break;
                case l_false:
                    IF_VERBOSE(2, verbose_stream() << "conflict: " << th.conflict_minimize() << "\n";);
                    if (!th.conflict_minimize().get_infinity().is_zero()) {
                        // bounds is not in the core. The context is unsat.
                        m_upper[i] = m_lower[i];
                        return l_false;
                    }
                    else {
                        m_upper[i] = std::min(m_upper[i], th.conflict_minimize());
                    }
                    break;
                default:
                    th.enable_record_conflict(0);
                    return l_undef;
                }
                th.enable_record_conflict(0);
                progress = true;
            }
        }
        if (m_cancel) {
            return l_undef;
        }
        if (!progress) {
            return l_false;
        }
        return l_true;
    }

    /**
       Takes solver with hard constraints added.
       Returns an optimal assignment to objective functions.
    */
    lbool optsmt::operator()(opt_solver& solver) {
        if (m_objs.empty()) {
            return l_true;
        }
        lbool is_sat = l_true;
        m_s = &solver;
        solver.reset_objectives();
        m_vars.reset();

        {
            // force base level
            solver::scoped_push _push(solver);
        }

        for (unsigned i = 0; i < m_objs.size(); ++i) {
            m_vars.push_back(solver.add_objective(m_objs[i].get()));
        }            

        if (m_engine == symbol("farkas")) {
            is_sat = farkas_opt();
        }
        else {
            is_sat = basic_opt();
        }

        IF_VERBOSE(1, verbose_stream() << "is-sat: " << is_sat << std::endl;
                   display_assignment(verbose_stream()););

        return is_sat;
    }

    inf_eps optsmt::get_value(unsigned i) const {
        return m_is_max[i]?m_lower[i]:-m_lower[i];
    }

    inf_eps optsmt::get_lower(unsigned i) const {
        return m_is_max[i]?m_lower[i]:-m_upper[i];
    }

    inf_eps optsmt::get_upper(unsigned i) const {
        return m_is_max[i]?m_upper[i]:-m_lower[i];
    }

    void optsmt::get_model(model_ref& mdl) {
        mdl = m_model.get();
    }

    // force lower_bound(i) <= objective_value(i)    
    void optsmt::commit_assignment(unsigned i) {
        inf_eps mid(0);

        // TBD: case analysis should be revisited
        rational hi = get_upper(i).get_infinity();
        rational lo = get_lower(i).get_infinity(); 
        if (hi.is_pos() && !lo.is_neg()) {
            mid = get_lower(i);
        }
        else if (hi.is_pos() && lo.is_neg()) {
            mid = inf_eps(0);
        }
        else if (hi.is_pos() && lo.is_pos()) {
            mid = inf_eps(0); // TBD: get a value from a model?
        }
        else if (hi.is_neg() && lo.is_neg()) {
            mid = inf_eps(0); // TBD: get a value from a model?
        }
        else {
            mid = hi;
        }
        m_lower[i] = mid;
        m_upper[i] = mid;
        TRACE("opt", tout << "set lower bound of "; display_objective(tout, i) << " to: " << mid << "\n";
              tout << get_lower(i) << ":" << get_upper(i) << "\n";);    
        // Only assert bounds for bounded objectives
        if (mid.get_infinity().is_zero())
            m_s->assert_expr(m_s->mk_ge(i, mid));
    }

    std::ostream& optsmt::display_objective(std::ostream& out, unsigned i) const {
        bool is_max = m_is_max[i];
        inf_eps val = get_value(i);
        expr_ref obj(m_objs[i], m);
        if (!is_max) {
            arith_util a(m);
            th_rewriter rw(m);
            obj = a.mk_uminus(obj);
            rw(obj, obj);
        }
        return out << obj;
    }

    void optsmt::display_assignment(std::ostream& out) const {
        unsigned sz = m_objs.size();
        for (unsigned i = 0; i < sz; ++i) {
            display_objective(out, i) << " |-> " << get_value(i) << std::endl;                
        }        
    }

    void optsmt::display_range_assignment(std::ostream& out) const {
        unsigned sz = m_objs.size();
        for (unsigned i = 0; i < sz; ++i) {
            display_objective(out, i) << " |-> [" << get_lower(i) << ":" << get_upper(i) << "]" << std::endl;                
        }        
    }

    void optsmt::add(app* t, bool is_max) {
        expr_ref t1(t, m), t2(m);
        th_rewriter rw(m);
        if (!is_max) {
            arith_util a(m);
            t1 = a.mk_uminus(t);
        }
        rw(t1, t2);
        SASSERT(is_app(t2));
        m_objs.push_back(to_app(t2));
        m_is_max.push_back(is_max);
        m_lower.push_back(inf_eps(rational(-1),inf_rational(0)));
        m_upper.push_back(inf_eps(rational(1), inf_rational(0)));
    }

    void optsmt::updt_params(params_ref& p) {
        opt_params _p(p);
        m_engine = _p.engine();        
    }

}

