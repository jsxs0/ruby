/**********************************************************************

  vm_trace.c -

  $Author: ko1 $
  created at: Tue Aug 14 19:37:09 2012

  Copyright (C) 1993-2012 Yukihiro Matsumoto

**********************************************************************/

/*
 * This file include two parts:
 *
 * (1) set_trace_func internal mechanisms
 *     and C level API
 *
 * (2) Ruby level API
 *  (2-1) set_trace_func API
 *  (2-2) TracePoint API (not yet)
 *
 */

#include "eval_intern.h"
#include "internal.h"
#include "internal/bits.h"
#include "internal/class.h"
#include "internal/gc.h"
#include "internal/hash.h"
#include "internal/symbol.h"
#include "internal/thread.h"
#include "iseq.h"
#include "ruby/atomic.h"
#include "ruby/debug.h"
#include "vm_core.h"
#include "ruby/ractor.h"
#include "yjit.h"

#include "builtin.h"

static VALUE sym_default;

/* (1) trace mechanisms */

typedef struct rb_event_hook_struct {
    rb_event_hook_flag_t hook_flags;
    rb_event_flag_t events;
    rb_event_hook_func_t func;
    VALUE data;
    struct rb_event_hook_struct *next;

    struct {
        rb_thread_t *th;
        unsigned int target_line;
    } filter;
} rb_event_hook_t;

typedef void (*rb_event_hook_raw_arg_func_t)(VALUE data, const rb_trace_arg_t *arg);

#define MAX_EVENT_NUM 32

void
rb_hook_list_mark(rb_hook_list_t *hooks)
{
    rb_event_hook_t *hook = hooks->hooks;

    while (hook) {
        rb_gc_mark(hook->data);
        hook = hook->next;
    }
}

void
rb_hook_list_mark_and_update(rb_hook_list_t *hooks)
{
    rb_event_hook_t *hook = hooks->hooks;

    while (hook) {
        rb_gc_mark_and_move(&hook->data);
        hook = hook->next;
    }
}

static void clean_hooks(rb_hook_list_t *list);

void
rb_hook_list_free(rb_hook_list_t *hooks)
{
    hooks->need_clean = true;

    if (hooks->running == 0) {
        clean_hooks(hooks);
    }
}

/* ruby_vm_event_flags management */

void rb_clear_attr_ccs(void);
void rb_clear_bf_ccs(void);

static void
update_global_event_hook(rb_event_flag_t prev_events, rb_event_flag_t new_events)
{
    rb_event_flag_t new_iseq_events = new_events & ISEQ_TRACE_EVENTS;
    rb_event_flag_t enabled_iseq_events = ruby_vm_event_enabled_global_flags & ISEQ_TRACE_EVENTS;
    bool first_time_iseq_events_p = new_iseq_events & ~enabled_iseq_events;
    bool enable_c_call   = (prev_events & RUBY_EVENT_C_CALL)   == 0 && (new_events & RUBY_EVENT_C_CALL);
    bool enable_c_return = (prev_events & RUBY_EVENT_C_RETURN) == 0 && (new_events & RUBY_EVENT_C_RETURN);
    bool enable_call     = (prev_events & RUBY_EVENT_CALL)     == 0 && (new_events & RUBY_EVENT_CALL);
    bool enable_return   = (prev_events & RUBY_EVENT_RETURN)   == 0 && (new_events & RUBY_EVENT_RETURN);

    // Modify ISEQs or CCs to enable tracing
    if (first_time_iseq_events_p) {
        // write all ISeqs only when new events are added for the first time
        rb_iseq_trace_set_all(new_iseq_events | enabled_iseq_events);
    }
    // if c_call or c_return is activated
    else if (enable_c_call || enable_c_return) {
        rb_clear_attr_ccs();
    }
    else if (enable_call || enable_return) {
        rb_clear_bf_ccs();
    }

    ruby_vm_event_flags = new_events;
    ruby_vm_event_enabled_global_flags |= new_events;
    rb_objspace_set_event_hook(new_events);

    // Invalidate JIT code as needed
    if (first_time_iseq_events_p || enable_c_call || enable_c_return) {
        // Invalidate all code when ISEQs are modified to use trace_* insns above.
        // Also invalidate when enabling c_call or c_return because generated code
        // never fires these events.
        // Internal events fire inside C routines so don't need special handling.
        // Do this after event flags updates so other ractors see updated vm events
        // when they wake up.
        rb_yjit_tracing_invalidate_all();
    }
}

/* add/remove hooks */

static rb_event_hook_t *
alloc_event_hook(rb_event_hook_func_t func, rb_event_flag_t events, VALUE data, rb_event_hook_flag_t hook_flags)
{
    rb_event_hook_t *hook;

    if ((events & RUBY_INTERNAL_EVENT_MASK) && (events & ~RUBY_INTERNAL_EVENT_MASK)) {
        rb_raise(rb_eTypeError, "Can not specify normal event and internal event simultaneously.");
    }

    hook = ALLOC(rb_event_hook_t);
    hook->hook_flags = hook_flags;
    hook->events = events;
    hook->func = func;
    hook->data = data;

    /* no filters */
    hook->filter.th = NULL;
    hook->filter.target_line = 0;

    return hook;
}

static void
hook_list_connect(VALUE list_owner, rb_hook_list_t *list, rb_event_hook_t *hook, int global_p)
{
    rb_event_flag_t prev_events = list->events;
    hook->next = list->hooks;
    list->hooks = hook;
    list->events |= hook->events;

    if (global_p) {
        /* global hooks are root objects at GC mark. */
        update_global_event_hook(prev_events, list->events);
    }
    else {
        RB_OBJ_WRITTEN(list_owner, Qundef, hook->data);
    }
}

static void
connect_event_hook(const rb_execution_context_t *ec, rb_event_hook_t *hook)
{
    rb_hook_list_t *list = rb_ec_ractor_hooks(ec);
    hook_list_connect(Qundef, list, hook, TRUE);
}

static void
rb_threadptr_add_event_hook(const rb_execution_context_t *ec, rb_thread_t *th,
                            rb_event_hook_func_t func, rb_event_flag_t events, VALUE data, rb_event_hook_flag_t hook_flags)
{
    rb_event_hook_t *hook = alloc_event_hook(func, events, data, hook_flags);
    hook->filter.th = th;
    connect_event_hook(ec, hook);
}

void
rb_thread_add_event_hook(VALUE thval, rb_event_hook_func_t func, rb_event_flag_t events, VALUE data)
{
    rb_threadptr_add_event_hook(GET_EC(), rb_thread_ptr(thval), func, events, data, RUBY_EVENT_HOOK_FLAG_SAFE);
}

void
rb_add_event_hook(rb_event_hook_func_t func, rb_event_flag_t events, VALUE data)
{
    rb_add_event_hook2(func, events, data, RUBY_EVENT_HOOK_FLAG_SAFE);
}

void
rb_thread_add_event_hook2(VALUE thval, rb_event_hook_func_t func, rb_event_flag_t events, VALUE data, rb_event_hook_flag_t hook_flags)
{
    rb_threadptr_add_event_hook(GET_EC(), rb_thread_ptr(thval), func, events, data, hook_flags);
}

void
rb_add_event_hook2(rb_event_hook_func_t func, rb_event_flag_t events, VALUE data, rb_event_hook_flag_t hook_flags)
{
    rb_event_hook_t *hook = alloc_event_hook(func, events, data, hook_flags);
    connect_event_hook(GET_EC(), hook);
}

static void
clean_hooks(rb_hook_list_t *list)
{
    rb_event_hook_t *hook, **nextp = &list->hooks;
    rb_event_flag_t prev_events = list->events;

    VM_ASSERT(list->running == 0);
    VM_ASSERT(list->need_clean == true);

    list->events = 0;
    list->need_clean = false;

    while ((hook = *nextp) != 0) {
        if (hook->hook_flags & RUBY_EVENT_HOOK_FLAG_DELETED) {
            *nextp = hook->next;
            xfree(hook);
        }
        else {
            list->events |= hook->events; /* update active events */
            nextp = &hook->next;
        }
    }

    if (list->is_local) {
        if (list->events == 0) {
            /* local events */
            ruby_xfree(list);
        }
    }
    else {
        update_global_event_hook(prev_events, list->events);
    }
}

static void
clean_hooks_check(rb_hook_list_t *list)
{
    if (UNLIKELY(list->need_clean)) {
        if (list->running == 0) {
            clean_hooks(list);
        }
    }
}

#define MATCH_ANY_FILTER_TH ((rb_thread_t *)1)

/* if func is 0, then clear all funcs */
static int
remove_event_hook(const rb_execution_context_t *ec, const rb_thread_t *filter_th, rb_event_hook_func_t func, VALUE data)
{
    rb_hook_list_t *list = rb_ec_ractor_hooks(ec);
    int ret = 0;
    rb_event_hook_t *hook = list->hooks;

    while (hook) {
        if (func == 0 || hook->func == func) {
            if (hook->filter.th == filter_th || filter_th == MATCH_ANY_FILTER_TH) {
                if (UNDEF_P(data) || hook->data == data) {
                    hook->hook_flags |= RUBY_EVENT_HOOK_FLAG_DELETED;
                    ret+=1;
                    list->need_clean = true;
                }
            }
        }
        hook = hook->next;
    }

    clean_hooks_check(list);
    return ret;
}

static int
rb_threadptr_remove_event_hook(const rb_execution_context_t *ec, const rb_thread_t *filter_th, rb_event_hook_func_t func, VALUE data)
{
    return remove_event_hook(ec, filter_th, func, data);
}

int
rb_thread_remove_event_hook(VALUE thval, rb_event_hook_func_t func)
{
    return rb_threadptr_remove_event_hook(GET_EC(), rb_thread_ptr(thval), func, Qundef);
}

int
rb_thread_remove_event_hook_with_data(VALUE thval, rb_event_hook_func_t func, VALUE data)
{
    return rb_threadptr_remove_event_hook(GET_EC(), rb_thread_ptr(thval), func, data);
}

int
rb_remove_event_hook(rb_event_hook_func_t func)
{
    return remove_event_hook(GET_EC(), NULL, func, Qundef);
}

int
rb_remove_event_hook_with_data(rb_event_hook_func_t func, VALUE data)
{
    return remove_event_hook(GET_EC(), NULL, func, data);
}

void
rb_ec_clear_current_thread_trace_func(const rb_execution_context_t *ec)
{
    rb_threadptr_remove_event_hook(ec, rb_ec_thread_ptr(ec), 0, Qundef);
}

void
rb_ec_clear_all_trace_func(const rb_execution_context_t *ec)
{
    rb_threadptr_remove_event_hook(ec, MATCH_ANY_FILTER_TH, 0, Qundef);
}

/* invoke hooks */

static void
exec_hooks_body(const rb_execution_context_t *ec, rb_hook_list_t *list, const rb_trace_arg_t *trace_arg)
{
    rb_event_hook_t *hook;

    for (hook = list->hooks; hook; hook = hook->next) {
        if (!(hook->hook_flags & RUBY_EVENT_HOOK_FLAG_DELETED) &&
            (trace_arg->event & hook->events) &&
            (LIKELY(hook->filter.th == 0) || hook->filter.th == rb_ec_thread_ptr(ec)) &&
            (LIKELY(hook->filter.target_line == 0) || (hook->filter.target_line == (unsigned int)rb_vm_get_sourceline(ec->cfp)))) {
            if (!(hook->hook_flags & RUBY_EVENT_HOOK_FLAG_RAW_ARG)) {
                (*hook->func)(trace_arg->event, hook->data, trace_arg->self, trace_arg->id, trace_arg->klass);
            }
            else {
                (*((rb_event_hook_raw_arg_func_t)hook->func))(hook->data, trace_arg);
            }
        }
    }
}

static int
exec_hooks_precheck(const rb_execution_context_t *ec, rb_hook_list_t *list, const rb_trace_arg_t *trace_arg)
{
    if (list->events & trace_arg->event) {
        list->running++;
        return TRUE;
    }
    else {
        return FALSE;
    }
}

static void
exec_hooks_postcheck(const rb_execution_context_t *ec, rb_hook_list_t *list)
{
    list->running--;
    clean_hooks_check(list);
}

static void
exec_hooks_unprotected(const rb_execution_context_t *ec, rb_hook_list_t *list, const rb_trace_arg_t *trace_arg)
{
    if (exec_hooks_precheck(ec, list, trace_arg) == 0) return;
    exec_hooks_body(ec, list, trace_arg);
    exec_hooks_postcheck(ec, list);
}

static int
exec_hooks_protected(rb_execution_context_t *ec, rb_hook_list_t *list, const rb_trace_arg_t *trace_arg)
{
    enum ruby_tag_type state;
    volatile int raised;

    if (exec_hooks_precheck(ec, list, trace_arg) == 0) return 0;

    raised = rb_ec_reset_raised(ec);

    /* TODO: Support !RUBY_EVENT_HOOK_FLAG_SAFE hooks */

    EC_PUSH_TAG(ec);
    if ((state = EC_EXEC_TAG()) == TAG_NONE) {
        exec_hooks_body(ec, list, trace_arg);
    }
    EC_POP_TAG();

    exec_hooks_postcheck(ec, list);

    if (raised) {
        rb_ec_set_raised(ec);
    }

    return state;
}

// pop_p: Whether to pop the frame for the TracePoint when it throws.
void
rb_exec_event_hooks(rb_trace_arg_t *trace_arg, rb_hook_list_t *hooks, int pop_p)
{
    rb_execution_context_t *ec = trace_arg->ec;

    if (UNLIKELY(trace_arg->event & RUBY_INTERNAL_EVENT_MASK)) {
        if (ec->trace_arg && (ec->trace_arg->event & RUBY_INTERNAL_EVENT_MASK)) {
            /* skip hooks because this thread doing INTERNAL_EVENT */
        }
        else {
            rb_trace_arg_t *prev_trace_arg = ec->trace_arg;

            ec->trace_arg = trace_arg;
            /* only global hooks */
            exec_hooks_unprotected(ec, rb_ec_ractor_hooks(ec), trace_arg);
            ec->trace_arg = prev_trace_arg;
        }
    }
    else {
        if (ec->trace_arg == NULL && /* check reentrant */
            trace_arg->self != rb_mRubyVMFrozenCore /* skip special methods. TODO: remove it. */) {
            const VALUE errinfo = ec->errinfo;
            const VALUE old_recursive = ec->local_storage_recursive_hash;
            enum ruby_tag_type state = 0;

            /* setup */
            ec->local_storage_recursive_hash = ec->local_storage_recursive_hash_for_trace;
            ec->errinfo = Qnil;
            ec->trace_arg = trace_arg;

            /* kick hooks */
            if ((state = exec_hooks_protected(ec, hooks, trace_arg)) == TAG_NONE) {
                ec->errinfo = errinfo;
            }

            /* cleanup */
            ec->trace_arg = NULL;
            ec->local_storage_recursive_hash_for_trace = ec->local_storage_recursive_hash;
            ec->local_storage_recursive_hash = old_recursive;

            if (state) {
                if (pop_p) {
                    if (VM_FRAME_FINISHED_P(ec->cfp)) {
                        rb_vm_tag_jmpbuf_deinit(&ec->tag->buf);
                        ec->tag = ec->tag->prev;
                    }
                    rb_vm_pop_frame(ec);
                }
                EC_JUMP_TAG(ec, state);
            }
        }
    }
}

VALUE
rb_suppress_tracing(VALUE (*func)(VALUE), VALUE arg)
{
    volatile int raised;
    volatile VALUE result = Qnil;
    rb_execution_context_t *const ec = GET_EC();
    rb_vm_t *const vm = rb_ec_vm_ptr(ec);
    enum ruby_tag_type state;
    rb_trace_arg_t dummy_trace_arg;
    dummy_trace_arg.event = 0;

    if (!ec->trace_arg) {
        ec->trace_arg = &dummy_trace_arg;
    }

    raised = rb_ec_reset_raised(ec);

    EC_PUSH_TAG(ec);
    if (LIKELY((state = EC_EXEC_TAG()) == TAG_NONE)) {
        result = (*func)(arg);
    }
    else {
        (void)*&vm; /* suppress "clobbered" warning */
    }
    EC_POP_TAG();

    if (raised) {
        rb_ec_reset_raised(ec);
    }

    if (ec->trace_arg == &dummy_trace_arg) {
        ec->trace_arg = NULL;
    }

    if (state) {
#if defined RUBY_USE_SETJMPEX && RUBY_USE_SETJMPEX
        RB_GC_GUARD(result);
#endif
        EC_JUMP_TAG(ec, state);
    }

    return result;
}

static void call_trace_func(rb_event_flag_t, VALUE data, VALUE self, ID id, VALUE klass);

/* (2-1) set_trace_func (old API) */

/*
 * call-seq:
 *    set_trace_func(proc)    -> proc
 *    set_trace_func(nil)     -> nil
 *
 * Establishes _proc_ as the handler for tracing, or disables
 * tracing if the parameter is +nil+.
 *
 * *Note:* this method is obsolete, please use TracePoint instead.
 *
 * _proc_ takes up to six parameters:
 *
 * * an event name string
 * * a filename string
 * * a line number
 * * a method name symbol, or nil
 * * a binding, or nil
 * * the class, module, or nil
 *
 * _proc_ is invoked whenever an event occurs.
 *
 * Events are:
 *
 * <code>"c-call"</code>:: call a C-language routine
 * <code>"c-return"</code>:: return from a C-language routine
 * <code>"call"</code>:: call a Ruby method
 * <code>"class"</code>:: start a class or module definition
 * <code>"end"</code>:: finish a class or module definition
 * <code>"line"</code>:: execute code on a new line
 * <code>"raise"</code>:: raise an exception
 * <code>"return"</code>:: return from a Ruby method
 *
 * Tracing is disabled within the context of _proc_.
 *
 *   class Test
 *     def test
 *       a = 1
 *       b = 2
 *     end
 *   end
 *
 *   set_trace_func proc { |event, file, line, id, binding, class_or_module|
 *     printf "%8s %s:%-2d %16p %14p\n", event, file, line, id, class_or_module
 *   }
 *   t = Test.new
 *   t.test
 *
 * Produces:
 *
 *   c-return prog.rb:8   :set_trace_func         Kernel
 *       line prog.rb:11              nil            nil
 *     c-call prog.rb:11             :new          Class
 *     c-call prog.rb:11      :initialize    BasicObject
 *   c-return prog.rb:11      :initialize    BasicObject
 *   c-return prog.rb:11             :new          Class
 *       line prog.rb:12              nil            nil
 *       call prog.rb:2             :test           Test
 *       line prog.rb:3             :test           Test
 *       line prog.rb:4             :test           Test
 *     return prog.rb:5             :test           Test
 */

static VALUE
set_trace_func(VALUE obj, VALUE trace)
{
    rb_remove_event_hook(call_trace_func);

    if (NIL_P(trace)) {
        return Qnil;
    }

    if (!rb_obj_is_proc(trace)) {
        rb_raise(rb_eTypeError, "trace_func needs to be Proc");
    }

    rb_add_event_hook(call_trace_func, RUBY_EVENT_ALL, trace);
    return trace;
}

static void
thread_add_trace_func(rb_execution_context_t *ec, rb_thread_t *filter_th, VALUE trace)
{
    if (!rb_obj_is_proc(trace)) {
        rb_raise(rb_eTypeError, "trace_func needs to be Proc");
    }

    rb_threadptr_add_event_hook(ec, filter_th, call_trace_func, RUBY_EVENT_ALL, trace, RUBY_EVENT_HOOK_FLAG_SAFE);
}

/*
 *  call-seq:
 *     thr.add_trace_func(proc)    -> proc
 *
 *  Adds _proc_ as a handler for tracing.
 *
 *  See Thread#set_trace_func and Kernel#set_trace_func.
 */

static VALUE
thread_add_trace_func_m(VALUE obj, VALUE trace)
{
    thread_add_trace_func(GET_EC(), rb_thread_ptr(obj), trace);
    return trace;
}

/*
 *  call-seq:
 *     thr.set_trace_func(proc)    -> proc
 *     thr.set_trace_func(nil)     -> nil
 *
 *  Establishes _proc_ on _thr_ as the handler for tracing, or
 *  disables tracing if the parameter is +nil+.
 *
 *  See Kernel#set_trace_func.
 */

static VALUE
thread_set_trace_func_m(VALUE target_thread, VALUE trace)
{
    rb_execution_context_t *ec = GET_EC();
    rb_thread_t *target_th = rb_thread_ptr(target_thread);

    rb_threadptr_remove_event_hook(ec, target_th, call_trace_func, Qundef);

    if (NIL_P(trace)) {
        return Qnil;
    }
    else {
        thread_add_trace_func(ec, target_th, trace);
        return trace;
    }
}

static const char *
get_event_name(rb_event_flag_t event)
{
    switch (event) {
      case RUBY_EVENT_LINE:     return "line";
      case RUBY_EVENT_CLASS:    return "class";
      case RUBY_EVENT_END:      return "end";
      case RUBY_EVENT_CALL:     return "call";
      case RUBY_EVENT_RETURN:	return "return";
      case RUBY_EVENT_C_CALL:	return "c-call";
      case RUBY_EVENT_C_RETURN:	return "c-return";
      case RUBY_EVENT_RAISE:	return "raise";
      default:
        return "unknown";
    }
}

static ID
get_event_id(rb_event_flag_t event)
{
    ID id;

    switch (event) {
#define C(name, NAME) case RUBY_EVENT_##NAME: CONST_ID(id, #name); return id;
        C(line, LINE);
        C(class, CLASS);
        C(end, END);
        C(call, CALL);
        C(return, RETURN);
        C(c_call, C_CALL);
        C(c_return, C_RETURN);
        C(raise, RAISE);
        C(b_call, B_CALL);
        C(b_return, B_RETURN);
        C(thread_begin, THREAD_BEGIN);
        C(thread_end, THREAD_END);
        C(fiber_switch, FIBER_SWITCH);
        C(script_compiled, SCRIPT_COMPILED);
        C(rescue, RESCUE);
#undef C
      default:
        return 0;
    }
}

static void
get_path_and_lineno(const rb_execution_context_t *ec, const rb_control_frame_t *cfp, rb_event_flag_t event, VALUE *pathp, int *linep)
{
    cfp = rb_vm_get_ruby_level_next_cfp(ec, cfp);

    if (cfp) {
        const rb_iseq_t *iseq = cfp->iseq;
        *pathp = rb_iseq_path(iseq);

        if (event & (RUBY_EVENT_CLASS |
                     RUBY_EVENT_CALL  |
                     RUBY_EVENT_B_CALL)) {
            *linep = FIX2INT(rb_iseq_first_lineno(iseq));
        }
        else {
            *linep = rb_vm_get_sourceline(cfp);
        }
    }
    else {
        *pathp = Qnil;
        *linep = 0;
    }
}

static void
call_trace_func(rb_event_flag_t event, VALUE proc, VALUE self, ID id, VALUE klass)
{
    int line;
    VALUE filename;
    VALUE eventname = rb_str_new2(get_event_name(event));
    VALUE argv[6];
    const rb_execution_context_t *ec = GET_EC();

    get_path_and_lineno(ec, ec->cfp, event, &filename, &line);

    if (!klass) {
        rb_ec_frame_method_id_and_class(ec, &id, 0, &klass);
    }

    if (klass) {
        if (RB_TYPE_P(klass, T_ICLASS)) {
            klass = RBASIC(klass)->klass;
        }
        else if (RCLASS_SINGLETON_P(klass)) {
            klass = RCLASS_ATTACHED_OBJECT(klass);
        }
    }

    argv[0] = eventname;
    argv[1] = filename;
    argv[2] = INT2FIX(line);
    argv[3] = id ? ID2SYM(id) : Qnil;
    argv[4] = Qnil;
    if (self && (filename != Qnil) &&
        event != RUBY_EVENT_C_CALL &&
        event != RUBY_EVENT_C_RETURN &&
        (VM_FRAME_RUBYFRAME_P(ec->cfp) && imemo_type_p((VALUE)ec->cfp->iseq, imemo_iseq))) {
        argv[4] = rb_binding_new();
    }
    argv[5] = klass ? klass : Qnil;

    rb_proc_call_with_block(proc, 6, argv, Qnil);
}

/* (2-2) TracePoint API */

static VALUE rb_cTracePoint;

typedef struct rb_tp_struct {
    rb_event_flag_t events;
    int tracing; /* bool */
    rb_thread_t *target_th;
    VALUE local_target_set; /* Hash: target ->
                             * Qtrue (if target is iseq) or
                             * Qfalse (if target is bmethod)
                             */
    void (*func)(VALUE tpval, void *data);
    void *data;
    VALUE proc;
    rb_ractor_t *ractor;
    VALUE self;
} rb_tp_t;

static void
tp_mark(void *ptr)
{
    rb_tp_t *tp = ptr;
    rb_gc_mark(tp->proc);
    rb_gc_mark(tp->local_target_set);
    if (tp->target_th) rb_gc_mark(tp->target_th->self);
}

static const rb_data_type_t tp_data_type = {
    "tracepoint",
    {
        tp_mark,
        RUBY_TYPED_DEFAULT_FREE,
        NULL, // Nothing allocated externally, so don't need a memsize function
    },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED | RUBY_TYPED_EMBEDDABLE
};

static VALUE
tp_alloc(VALUE klass)
{
    rb_tp_t *tp;
    return TypedData_Make_Struct(klass, rb_tp_t, &tp_data_type, tp);
}

static rb_event_flag_t
symbol2event_flag(VALUE v)
{
    ID id;
    VALUE sym = rb_to_symbol_type(v);
    const rb_event_flag_t RUBY_EVENT_A_CALL =
        RUBY_EVENT_CALL | RUBY_EVENT_B_CALL | RUBY_EVENT_C_CALL;
    const rb_event_flag_t RUBY_EVENT_A_RETURN =
        RUBY_EVENT_RETURN | RUBY_EVENT_B_RETURN | RUBY_EVENT_C_RETURN;

#define C(name, NAME) CONST_ID(id, #name); if (sym == ID2SYM(id)) return RUBY_EVENT_##NAME
    C(line, LINE);
    C(class, CLASS);
    C(end, END);
    C(call, CALL);
    C(return, RETURN);
    C(c_call, C_CALL);
    C(c_return, C_RETURN);
    C(raise, RAISE);
    C(b_call, B_CALL);
    C(b_return, B_RETURN);
    C(thread_begin, THREAD_BEGIN);
    C(thread_end, THREAD_END);
    C(fiber_switch, FIBER_SWITCH);
    C(script_compiled, SCRIPT_COMPILED);
    C(rescue, RESCUE);

    /* joke */
    C(a_call, A_CALL);
    C(a_return, A_RETURN);
#undef C
    rb_raise(rb_eArgError, "unknown event: %"PRIsVALUE, rb_sym2str(sym));
}

static rb_tp_t *
tpptr(VALUE tpval)
{
    rb_tp_t *tp;
    TypedData_Get_Struct(tpval, rb_tp_t, &tp_data_type, tp);
    return tp;
}

static rb_trace_arg_t *
get_trace_arg(void)
{
    rb_trace_arg_t *trace_arg = GET_EC()->trace_arg;
    if (trace_arg == 0) {
        rb_raise(rb_eRuntimeError, "access from outside");
    }
    return trace_arg;
}

struct rb_trace_arg_struct *
rb_tracearg_from_tracepoint(VALUE tpval)
{
    return get_trace_arg();
}

rb_event_flag_t
rb_tracearg_event_flag(rb_trace_arg_t *trace_arg)
{
    return trace_arg->event;
}

VALUE
rb_tracearg_event(rb_trace_arg_t *trace_arg)
{
    return ID2SYM(get_event_id(trace_arg->event));
}

static void
fill_path_and_lineno(rb_trace_arg_t *trace_arg)
{
    if (UNDEF_P(trace_arg->path)) {
        get_path_and_lineno(trace_arg->ec, trace_arg->cfp, trace_arg->event, &trace_arg->path, &trace_arg->lineno);
    }
}

VALUE
rb_tracearg_lineno(rb_trace_arg_t *trace_arg)
{
    fill_path_and_lineno(trace_arg);
    return INT2FIX(trace_arg->lineno);
}
VALUE
rb_tracearg_path(rb_trace_arg_t *trace_arg)
{
    fill_path_and_lineno(trace_arg);
    return trace_arg->path;
}

static void
fill_id_and_klass(rb_trace_arg_t *trace_arg)
{
    if (!trace_arg->klass_solved) {
        if (!trace_arg->klass) {
            rb_vm_control_frame_id_and_class(trace_arg->cfp, &trace_arg->id, &trace_arg->called_id, &trace_arg->klass);
        }

        if (trace_arg->klass) {
            if (RB_TYPE_P(trace_arg->klass, T_ICLASS)) {
                trace_arg->klass = RBASIC(trace_arg->klass)->klass;
            }
        }
        else {
            trace_arg->klass = Qnil;
        }

        trace_arg->klass_solved = 1;
    }
}

VALUE
rb_tracearg_parameters(rb_trace_arg_t *trace_arg)
{
    switch (trace_arg->event) {
      case RUBY_EVENT_CALL:
      case RUBY_EVENT_RETURN:
      case RUBY_EVENT_B_CALL:
      case RUBY_EVENT_B_RETURN: {
        const rb_control_frame_t *cfp = rb_vm_get_ruby_level_next_cfp(trace_arg->ec, trace_arg->cfp);
        if (cfp) {
            int is_proc = 0;
            if (VM_FRAME_TYPE(cfp) == VM_FRAME_MAGIC_BLOCK && !VM_FRAME_LAMBDA_P(cfp)) {
                is_proc = 1;
            }
            return rb_iseq_parameters(cfp->iseq, is_proc);
        }
        break;
      }
      case RUBY_EVENT_C_CALL:
      case RUBY_EVENT_C_RETURN: {
        fill_id_and_klass(trace_arg);
        if (trace_arg->klass && trace_arg->id) {
            const rb_method_entry_t *me;
            VALUE iclass = Qnil;
            me = rb_method_entry_without_refinements(trace_arg->klass, trace_arg->called_id, &iclass);
            if (!me) {
                me = rb_method_entry_without_refinements(trace_arg->klass, trace_arg->id, &iclass);
            }
            return rb_unnamed_parameters(rb_method_entry_arity(me));
        }
        break;
      }
      case RUBY_EVENT_RAISE:
      case RUBY_EVENT_LINE:
      case RUBY_EVENT_CLASS:
      case RUBY_EVENT_END:
      case RUBY_EVENT_SCRIPT_COMPILED:
      case RUBY_EVENT_RESCUE:
        rb_raise(rb_eRuntimeError, "not supported by this event");
        break;
    }
    return Qnil;
}

VALUE
rb_tracearg_method_id(rb_trace_arg_t *trace_arg)
{
    fill_id_and_klass(trace_arg);
    return trace_arg->id ? ID2SYM(trace_arg->id) : Qnil;
}

VALUE
rb_tracearg_callee_id(rb_trace_arg_t *trace_arg)
{
    fill_id_and_klass(trace_arg);
    return trace_arg->called_id ? ID2SYM(trace_arg->called_id) : Qnil;
}

VALUE
rb_tracearg_defined_class(rb_trace_arg_t *trace_arg)
{
    fill_id_and_klass(trace_arg);
    return trace_arg->klass;
}

VALUE
rb_tracearg_binding(rb_trace_arg_t *trace_arg)
{
    rb_control_frame_t *cfp;
    switch (trace_arg->event) {
      case RUBY_EVENT_C_CALL:
      case RUBY_EVENT_C_RETURN:
        return Qnil;
    }
    cfp = rb_vm_get_binding_creatable_next_cfp(trace_arg->ec, trace_arg->cfp);

    if (cfp && imemo_type_p((VALUE)cfp->iseq, imemo_iseq)) {
        return rb_vm_make_binding(trace_arg->ec, cfp);
    }
    else {
        return Qnil;
    }
}

VALUE
rb_tracearg_self(rb_trace_arg_t *trace_arg)
{
    return trace_arg->self;
}

VALUE
rb_tracearg_return_value(rb_trace_arg_t *trace_arg)
{
    if (trace_arg->event & (RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN | RUBY_EVENT_B_RETURN)) {
        /* ok */
    }
    else {
        rb_raise(rb_eRuntimeError, "not supported by this event");
    }
    if (UNDEF_P(trace_arg->data)) {
        rb_bug("rb_tracearg_return_value: unreachable");
    }
    return trace_arg->data;
}

VALUE
rb_tracearg_raised_exception(rb_trace_arg_t *trace_arg)
{
    if (trace_arg->event & (RUBY_EVENT_RAISE | RUBY_EVENT_RESCUE)) {
        /* ok */
    }
    else {
        rb_raise(rb_eRuntimeError, "not supported by this event");
    }
    if (UNDEF_P(trace_arg->data)) {
        rb_bug("rb_tracearg_raised_exception: unreachable");
    }
    return trace_arg->data;
}

VALUE
rb_tracearg_eval_script(rb_trace_arg_t *trace_arg)
{
    VALUE data = trace_arg->data;

    if (trace_arg->event & (RUBY_EVENT_SCRIPT_COMPILED)) {
        /* ok */
    }
    else {
        rb_raise(rb_eRuntimeError, "not supported by this event");
    }
    if (UNDEF_P(data)) {
        rb_bug("rb_tracearg_raised_exception: unreachable");
    }
    if (rb_obj_is_iseq(data)) {
        return Qnil;
    }
    else {
        VM_ASSERT(RB_TYPE_P(data, T_ARRAY));
        /* [src, iseq] */
        return RARRAY_AREF(data, 0);
    }
}

VALUE
rb_tracearg_instruction_sequence(rb_trace_arg_t *trace_arg)
{
    VALUE data = trace_arg->data;

    if (trace_arg->event & (RUBY_EVENT_SCRIPT_COMPILED)) {
        /* ok */
    }
    else {
        rb_raise(rb_eRuntimeError, "not supported by this event");
    }
    if (UNDEF_P(data)) {
        rb_bug("rb_tracearg_raised_exception: unreachable");
    }

    if (rb_obj_is_iseq(data)) {
        return rb_iseqw_new((const rb_iseq_t *)data);
    }
    else {
        VM_ASSERT(RB_TYPE_P(data, T_ARRAY));
        VM_ASSERT(rb_obj_is_iseq(RARRAY_AREF(data, 1)));

        /* [src, iseq] */
        return rb_iseqw_new((const rb_iseq_t *)RARRAY_AREF(data, 1));
    }
}

VALUE
rb_tracearg_object(rb_trace_arg_t *trace_arg)
{
    if (trace_arg->event & (RUBY_INTERNAL_EVENT_NEWOBJ | RUBY_INTERNAL_EVENT_FREEOBJ)) {
        /* ok */
    }
    else {
        rb_raise(rb_eRuntimeError, "not supported by this event");
    }
    if (UNDEF_P(trace_arg->data)) {
        rb_bug("rb_tracearg_object: unreachable");
    }
    return trace_arg->data;
}

static VALUE
tracepoint_attr_event(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_event(get_trace_arg());
}

static VALUE
tracepoint_attr_lineno(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_lineno(get_trace_arg());
}
static VALUE
tracepoint_attr_path(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_path(get_trace_arg());
}

static VALUE
tracepoint_attr_parameters(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_parameters(get_trace_arg());
}

static VALUE
tracepoint_attr_method_id(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_method_id(get_trace_arg());
}

static VALUE
tracepoint_attr_callee_id(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_callee_id(get_trace_arg());
}

static VALUE
tracepoint_attr_defined_class(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_defined_class(get_trace_arg());
}

static VALUE
tracepoint_attr_binding(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_binding(get_trace_arg());
}

static VALUE
tracepoint_attr_self(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_self(get_trace_arg());
}

static VALUE
tracepoint_attr_return_value(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_return_value(get_trace_arg());
}

static VALUE
tracepoint_attr_raised_exception(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_raised_exception(get_trace_arg());
}

static VALUE
tracepoint_attr_eval_script(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_eval_script(get_trace_arg());
}

static VALUE
tracepoint_attr_instruction_sequence(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracearg_instruction_sequence(get_trace_arg());
}

static void
tp_call_trace(VALUE tpval, rb_trace_arg_t *trace_arg)
{
    rb_tp_t *tp = tpptr(tpval);

    if (tp->func) {
        (*tp->func)(tpval, tp->data);
    }
    else {
        if (tp->ractor == NULL || tp->ractor == GET_RACTOR()) {
            rb_proc_call_with_block((VALUE)tp->proc, 1, &tpval, Qnil);
        }
    }
}

VALUE
rb_tracepoint_enable(VALUE tpval)
{
    rb_tp_t *tp;
    tp = tpptr(tpval);

    if (tp->local_target_set != Qfalse) {
        rb_raise(rb_eArgError, "can't nest-enable a targeting TracePoint");
    }

    if (tp->tracing) {
        return Qundef;
    }

    if (tp->target_th) {
        rb_thread_add_event_hook2(tp->target_th->self, (rb_event_hook_func_t)tp_call_trace, tp->events, tpval,
                                  RUBY_EVENT_HOOK_FLAG_SAFE | RUBY_EVENT_HOOK_FLAG_RAW_ARG);
    }
    else {
        rb_add_event_hook2((rb_event_hook_func_t)tp_call_trace, tp->events, tpval,
                           RUBY_EVENT_HOOK_FLAG_SAFE | RUBY_EVENT_HOOK_FLAG_RAW_ARG);
    }
    tp->tracing = 1;
    return Qundef;
}

static const rb_iseq_t *
iseq_of(VALUE target)
{
    VALUE iseqv = rb_funcall(rb_cISeq, rb_intern("of"), 1, target);
    if (NIL_P(iseqv)) {
        rb_raise(rb_eArgError, "specified target is not supported");
    }
    else {
        return rb_iseqw_to_iseq(iseqv);
    }
}

const rb_method_definition_t *rb_method_def(VALUE method); /* proc.c */

static VALUE
rb_tracepoint_enable_for_target(VALUE tpval, VALUE target, VALUE target_line)
{
    rb_tp_t *tp = tpptr(tpval);
    const rb_iseq_t *iseq = iseq_of(target);
    int n = 0;
    unsigned int line = 0;
    bool target_bmethod = false;

    if (tp->tracing > 0) {
        rb_raise(rb_eArgError, "can't nest-enable a targeting TracePoint");
    }

    if (!NIL_P(target_line)) {
        if ((tp->events & RUBY_EVENT_LINE) == 0) {
            rb_raise(rb_eArgError, "target_line is specified, but line event is not specified");
        }
        else {
            line = NUM2UINT(target_line);
        }
    }

    VM_ASSERT(tp->local_target_set == Qfalse);
    RB_OBJ_WRITE(tpval, &tp->local_target_set, rb_obj_hide(rb_ident_hash_new()));

    /* bmethod */
    if (rb_obj_is_method(target)) {
        rb_method_definition_t *def = (rb_method_definition_t *)rb_method_def(target);
        if (def->type == VM_METHOD_TYPE_BMETHOD &&
            (tp->events & (RUBY_EVENT_CALL | RUBY_EVENT_RETURN))) {
            if (def->body.bmethod.hooks == NULL) {
                def->body.bmethod.hooks = ZALLOC(rb_hook_list_t);
                def->body.bmethod.hooks->is_local = true;
            }
            rb_hook_list_connect_tracepoint(target, def->body.bmethod.hooks, tpval, 0);
            rb_hash_aset(tp->local_target_set, target, Qfalse);
            target_bmethod = true;

            n++;
        }
    }

    /* iseq */
    n += rb_iseq_add_local_tracepoint_recursively(iseq, tp->events, tpval, line, target_bmethod);
    rb_hash_aset(tp->local_target_set, (VALUE)iseq, Qtrue);

    if ((tp->events & (RUBY_EVENT_CALL | RUBY_EVENT_RETURN)) &&
        iseq->body->builtin_attrs & BUILTIN_ATTR_SINGLE_NOARG_LEAF) {
        rb_clear_bf_ccs();
    }

    if (n == 0) {
        rb_raise(rb_eArgError, "can not enable any hooks");
    }

    rb_yjit_tracing_invalidate_all();

    ruby_vm_event_local_num++;

    tp->tracing = 1;

    return Qnil;
}

static int
disable_local_event_iseq_i(VALUE target, VALUE iseq_p, VALUE tpval)
{
    if (iseq_p) {
        rb_iseq_remove_local_tracepoint_recursively((rb_iseq_t *)target, tpval);
    }
    else {
        /* bmethod */
        rb_method_definition_t *def = (rb_method_definition_t *)rb_method_def(target);
        rb_hook_list_t *hooks = def->body.bmethod.hooks;
        VM_ASSERT(hooks != NULL);
        rb_hook_list_remove_tracepoint(hooks, tpval);

        if (hooks->events == 0) {
            rb_hook_list_free(def->body.bmethod.hooks);
            def->body.bmethod.hooks = NULL;
        }
    }
    return ST_CONTINUE;
}

VALUE
rb_tracepoint_disable(VALUE tpval)
{
    rb_tp_t *tp;

    tp = tpptr(tpval);

    if (tp->local_target_set) {
        rb_hash_foreach(tp->local_target_set, disable_local_event_iseq_i, tpval);
        RB_OBJ_WRITE(tpval, &tp->local_target_set, Qfalse);
        ruby_vm_event_local_num--;
    }
    else {
        if (tp->target_th) {
            rb_thread_remove_event_hook_with_data(tp->target_th->self, (rb_event_hook_func_t)tp_call_trace, tpval);
        }
        else {
            rb_remove_event_hook_with_data((rb_event_hook_func_t)tp_call_trace, tpval);
        }
    }
    tp->tracing = 0;
    tp->target_th = NULL;
    return Qundef;
}

void
rb_hook_list_connect_tracepoint(VALUE target, rb_hook_list_t *list, VALUE tpval, unsigned int target_line)
{
    rb_tp_t *tp = tpptr(tpval);
    rb_event_hook_t *hook = alloc_event_hook((rb_event_hook_func_t)tp_call_trace, tp->events & ISEQ_TRACE_EVENTS, tpval,
                                             RUBY_EVENT_HOOK_FLAG_SAFE | RUBY_EVENT_HOOK_FLAG_RAW_ARG);
    hook->filter.target_line = target_line;
    hook_list_connect(target, list, hook, FALSE);
}

void
rb_hook_list_remove_tracepoint(rb_hook_list_t *list, VALUE tpval)
{
    rb_event_hook_t *hook = list->hooks;
    rb_event_flag_t events = 0;

    while (hook) {
        if (hook->data == tpval) {
            hook->hook_flags |= RUBY_EVENT_HOOK_FLAG_DELETED;
            list->need_clean = true;
        }
        else if ((hook->hook_flags & RUBY_EVENT_HOOK_FLAG_DELETED) == 0) {
            events |= hook->events;
        }
        hook = hook->next;
    }

    list->events = events;
}

static VALUE
tracepoint_enable_m(rb_execution_context_t *ec, VALUE tpval, VALUE target, VALUE target_line, VALUE target_thread)
{
    rb_tp_t *tp = tpptr(tpval);
    int previous_tracing = tp->tracing;

    if (target_thread == sym_default) {
        if (rb_block_given_p() && NIL_P(target) && NIL_P(target_line)) {
            target_thread = rb_thread_current();
        }
        else {
            target_thread = Qnil;
        }
    }

    /* check target_thread */
    if (RTEST(target_thread)) {
        if (tp->target_th) {
            rb_raise(rb_eArgError, "can not override target_thread filter");
        }
        tp->target_th = rb_thread_ptr(target_thread);

        RUBY_ASSERT(tp->target_th->self == target_thread);
        RB_OBJ_WRITTEN(tpval, Qundef, target_thread);
    }
    else {
        tp->target_th = NULL;
    }

    if (NIL_P(target)) {
        if (!NIL_P(target_line)) {
            rb_raise(rb_eArgError, "only target_line is specified");
        }
        rb_tracepoint_enable(tpval);
    }
    else {
        rb_tracepoint_enable_for_target(tpval, target, target_line);
    }

    if (rb_block_given_p()) {
        return rb_ensure(rb_yield, Qundef,
                         previous_tracing ? rb_tracepoint_enable : rb_tracepoint_disable,
                         tpval);
    }
    else {
        return RBOOL(previous_tracing);
    }
}

static VALUE
tracepoint_disable_m(rb_execution_context_t *ec, VALUE tpval)
{
    rb_tp_t *tp = tpptr(tpval);
    int previous_tracing = tp->tracing;

    if (rb_block_given_p()) {
        if (tp->local_target_set != Qfalse) {
            rb_raise(rb_eArgError, "can't disable a targeting TracePoint in a block");
        }

        rb_tracepoint_disable(tpval);
        return rb_ensure(rb_yield, Qundef,
                         previous_tracing ? rb_tracepoint_enable : rb_tracepoint_disable,
                         tpval);
    }
    else {
        rb_tracepoint_disable(tpval);
        return RBOOL(previous_tracing);
    }
}

VALUE
rb_tracepoint_enabled_p(VALUE tpval)
{
    rb_tp_t *tp = tpptr(tpval);
    return RBOOL(tp->tracing);
}

static VALUE
tracepoint_enabled_p(rb_execution_context_t *ec, VALUE tpval)
{
    return rb_tracepoint_enabled_p(tpval);
}

static VALUE
tracepoint_new(VALUE klass, rb_thread_t *target_th, rb_event_flag_t events, void (func)(VALUE, void*), void *data, VALUE proc)
{
    VALUE tpval = tp_alloc(klass);
    rb_tp_t *tp;
    TypedData_Get_Struct(tpval, rb_tp_t, &tp_data_type, tp);

    RB_OBJ_WRITE(tpval, &tp->proc, proc);
    tp->ractor = rb_ractor_shareable_p(proc) ? NULL : GET_RACTOR();
    tp->func = func;
    tp->data = data;
    tp->events = events;
    tp->self = tpval;

    return tpval;
}

VALUE
rb_tracepoint_new(VALUE target_thval, rb_event_flag_t events, void (*func)(VALUE, void *), void *data)
{
    rb_thread_t *target_th = NULL;

    if (RTEST(target_thval)) {
        target_th = rb_thread_ptr(target_thval);
        /* TODO: Test it!
         * Warning: This function is not tested.
         */
    }
    return tracepoint_new(rb_cTracePoint, target_th, events, func, data, Qundef);
}

static VALUE
tracepoint_new_s(rb_execution_context_t *ec, VALUE self, VALUE args)
{
    rb_event_flag_t events = 0;
    long i;
    long argc = RARRAY_LEN(args);

    if (argc > 0) {
        for (i=0; i<argc; i++) {
            events |= symbol2event_flag(RARRAY_AREF(args, i));
        }
    }
    else {
        events = RUBY_EVENT_TRACEPOINT_ALL;
    }

    if (!rb_block_given_p()) {
        rb_raise(rb_eArgError, "must be called with a block");
    }

    return tracepoint_new(self, 0, events, 0, 0, rb_block_proc());
}

static VALUE
tracepoint_trace_s(rb_execution_context_t *ec, VALUE self, VALUE args)
{
    VALUE trace = tracepoint_new_s(ec, self, args);
    rb_tracepoint_enable(trace);
    return trace;
}

static VALUE
tracepoint_inspect(rb_execution_context_t *ec, VALUE self)
{
    rb_tp_t *tp = tpptr(self);
    rb_trace_arg_t *trace_arg = GET_EC()->trace_arg;

    if (trace_arg) {
        switch (trace_arg->event) {
          case RUBY_EVENT_LINE:
            {
                VALUE sym = rb_tracearg_method_id(trace_arg);
                if (NIL_P(sym))
                    break;
                return rb_sprintf("#<TracePoint:%"PRIsVALUE" %"PRIsVALUE":%d in '%"PRIsVALUE"'>",
                                  rb_tracearg_event(trace_arg),
                                  rb_tracearg_path(trace_arg),
                                  FIX2INT(rb_tracearg_lineno(trace_arg)),
                                  sym);
            }
          case RUBY_EVENT_CALL:
          case RUBY_EVENT_C_CALL:
          case RUBY_EVENT_RETURN:
          case RUBY_EVENT_C_RETURN:
            return rb_sprintf("#<TracePoint:%"PRIsVALUE" '%"PRIsVALUE"' %"PRIsVALUE":%d>",
                              rb_tracearg_event(trace_arg),
                              rb_tracearg_method_id(trace_arg),
                              rb_tracearg_path(trace_arg),
                              FIX2INT(rb_tracearg_lineno(trace_arg)));
          case RUBY_EVENT_THREAD_BEGIN:
          case RUBY_EVENT_THREAD_END:
            return rb_sprintf("#<TracePoint:%"PRIsVALUE" %"PRIsVALUE">",
                              rb_tracearg_event(trace_arg),
                              rb_tracearg_self(trace_arg));
          default:
            break;
        }
        return rb_sprintf("#<TracePoint:%"PRIsVALUE" %"PRIsVALUE":%d>",
                          rb_tracearg_event(trace_arg),
                          rb_tracearg_path(trace_arg),
                          FIX2INT(rb_tracearg_lineno(trace_arg)));
    }
    else {
        return rb_sprintf("#<TracePoint:%s>", tp->tracing ? "enabled" : "disabled");
    }
}

static void
tracepoint_stat_event_hooks(VALUE hash, VALUE key, rb_event_hook_t *hook)
{
    int active = 0, deleted = 0;

    while (hook) {
        if (hook->hook_flags & RUBY_EVENT_HOOK_FLAG_DELETED) {
            deleted++;
        }
        else {
            active++;
        }
        hook = hook->next;
    }

    rb_hash_aset(hash, key, rb_ary_new3(2, INT2FIX(active), INT2FIX(deleted)));
}

static VALUE
tracepoint_stat_s(rb_execution_context_t *ec, VALUE self)
{
    rb_vm_t *vm = GET_VM();
    VALUE stat = rb_hash_new();

    tracepoint_stat_event_hooks(stat, vm->self, rb_ec_ractor_hooks(ec)->hooks);
    /* TODO: thread local hooks */

    return stat;
}

static VALUE
disallow_reentry(VALUE val)
{
    rb_trace_arg_t *arg = (rb_trace_arg_t *)val;
    rb_execution_context_t *ec = GET_EC();
    if (ec->trace_arg != NULL) rb_bug("should be NULL, but %p", (void *)ec->trace_arg);
    ec->trace_arg = arg;
    return Qnil;
}

static VALUE
tracepoint_allow_reentry(rb_execution_context_t *ec, VALUE self)
{
    const rb_trace_arg_t *arg = ec->trace_arg;
    if (arg == NULL) rb_raise(rb_eRuntimeError, "No need to allow reentrance.");
    ec->trace_arg = NULL;
    return rb_ensure(rb_yield, Qnil, disallow_reentry, (VALUE)arg);
}

#include "trace_point.rbinc"

/* This function is called from inits.c */
void
Init_vm_trace(void)
{
    sym_default = ID2SYM(rb_intern_const("default"));

    /* trace_func */
    rb_define_global_function("set_trace_func", set_trace_func, 1);
    rb_define_method(rb_cThread, "set_trace_func", thread_set_trace_func_m, 1);
    rb_define_method(rb_cThread, "add_trace_func", thread_add_trace_func_m, 1);

    rb_cTracePoint = rb_define_class("TracePoint", rb_cObject);
    rb_undef_alloc_func(rb_cTracePoint);
}

/*
 * Ruby actually has two separate mechanisms for enqueueing work from contexts
 * where it is not safe to run Ruby code, to run later on when it is safe. One
 * is async-signal-safe but more limited, and accessed through the
 * `rb_postponed_job_preregister` and `rb_postponed_job_trigger` functions. The
 * other is more flexible but cannot be used in signal handlers, and is accessed
 * through the `rb_workqueue_register` function.
 *
 * The postponed job functions form part of Ruby's extension API, but the
 * workqueue functions are for internal use only.
 */

struct rb_workqueue_job {
    struct ccan_list_node jnode; /* <=> vm->workqueue */
    rb_postponed_job_func_t func;
    void *data;
};

// Used for VM memsize reporting. Returns the size of a list of rb_workqueue_job
// structs. Defined here because the struct definition lives here as well.
size_t
rb_vm_memsize_workqueue(struct ccan_list_head *workqueue)
{
    struct rb_workqueue_job *work = 0;
    size_t size = 0;

    ccan_list_for_each(workqueue, work, jnode) {
        size += sizeof(struct rb_workqueue_job);
    }

    return size;
}

/*
 * thread-safe and called from non-Ruby thread
 * returns FALSE on failure (ENOMEM), TRUE otherwise
 */
int
rb_workqueue_register(unsigned flags, rb_postponed_job_func_t func, void *data)
{
    struct rb_workqueue_job *wq_job = malloc(sizeof(*wq_job));
    rb_vm_t *vm = GET_VM();

    if (!wq_job) return FALSE;
    wq_job->func = func;
    wq_job->data = data;

    rb_nativethread_lock_lock(&vm->workqueue_lock);
    ccan_list_add_tail(&vm->workqueue, &wq_job->jnode);
    rb_nativethread_lock_unlock(&vm->workqueue_lock);

    // TODO: current implementation affects only main ractor
    RUBY_VM_SET_POSTPONED_JOB_INTERRUPT(rb_vm_main_ractor_ec(vm));

    return TRUE;
}

#define PJOB_TABLE_SIZE              (sizeof(rb_atomic_t) * CHAR_BIT)
/* pre-registered jobs table, for async-safe jobs */
typedef struct rb_postponed_job_queue {
    struct {
        rb_postponed_job_func_t func;
        void *data;
    } table[PJOB_TABLE_SIZE];
    /* Bits in this are set when the corresponding entry in prereg_table has non-zero
     * triggered_count; i.e. somebody called rb_postponed_job_trigger */
    rb_atomic_t triggered_bitset;
} rb_postponed_job_queues_t;

void
rb_vm_postponed_job_queue_init(rb_vm_t *vm)
{
    /* use mimmalloc; postponed job registration is a dependency of objspace, so this gets
     * called _VERY_ early inside Init_BareVM */
    rb_postponed_job_queues_t *pjq = ruby_mimmalloc(sizeof(rb_postponed_job_queues_t));
    pjq->triggered_bitset = 0;
    memset(pjq->table, 0, sizeof(pjq->table));
    vm->postponed_job_queue = pjq;
}

static rb_execution_context_t *
get_valid_ec(rb_vm_t *vm)
{
    rb_execution_context_t *ec = rb_current_execution_context(false);
    if (ec == NULL) ec = rb_vm_main_ractor_ec(vm);
    return ec;
}

void
rb_vm_postponed_job_atfork(void)
{
    rb_vm_t *vm = GET_VM();
    rb_postponed_job_queues_t *pjq = vm->postponed_job_queue;
    /* make sure we set the interrupt flag on _this_ thread if we carried any pjobs over
     * from the other side of the fork */
    if (pjq->triggered_bitset) {
        RUBY_VM_SET_POSTPONED_JOB_INTERRUPT(get_valid_ec(vm));
    }

}

/* Frees the memory managed by the postponed job infrastructure at shutdown */
void
rb_vm_postponed_job_free(void)
{
    rb_vm_t *vm = GET_VM();
    ruby_xfree(vm->postponed_job_queue);
    vm->postponed_job_queue = NULL;
}

// Used for VM memsize reporting. Returns the total size of the postponed job
// queue infrastructure.
size_t
rb_vm_memsize_postponed_job_queue(void)
{
    return sizeof(rb_postponed_job_queues_t);
}


rb_postponed_job_handle_t
rb_postponed_job_preregister(unsigned int flags, rb_postponed_job_func_t func, void *data)
{
    /* The doc comments say that this function should be called under the GVL, because
     * that is actually required to get the guarantee that "if a given (func, data) pair
     * was already pre-registered, this method will return the same handle instance".
     *
     * However, the actual implementation here is called without the GVL, from inside
     * rb_postponed_job_register, to support that legacy interface. In the presence
     * of concurrent calls to both _preregister and _register functions on the same
     * func, however, the data may get mixed up between them. */

    rb_postponed_job_queues_t *pjq = GET_VM()->postponed_job_queue;
    for (unsigned int i = 0; i < PJOB_TABLE_SIZE; i++) {
        /* Try and set this slot to equal `func` */
        rb_postponed_job_func_t existing_func = (rb_postponed_job_func_t)(uintptr_t)RUBY_ATOMIC_PTR_CAS(pjq->table[i].func, NULL, (void *)(uintptr_t)func);
        if (existing_func == NULL || existing_func == func) {
            /* Either this slot was NULL, and we set it to func, or, this slot was already equal to func.
             * In either case, clobber the data with our data. Note that concurrent calls to
             * rb_postponed_job_register with the same func & different data will result in either of the
             * datas being written */
            RUBY_ATOMIC_PTR_EXCHANGE(pjq->table[i].data, data);
            return (rb_postponed_job_handle_t)i;
        }
        else {
            /* Try the next slot if this one already has a func in it */
            continue;
        }
    }

    /* full */
    return POSTPONED_JOB_HANDLE_INVALID;
}

void
rb_postponed_job_trigger(rb_postponed_job_handle_t h)
{
    rb_vm_t *vm = GET_VM();
    rb_postponed_job_queues_t *pjq = vm->postponed_job_queue;

    RUBY_ATOMIC_OR(pjq->triggered_bitset, (((rb_atomic_t)1UL) << h));
    RUBY_VM_SET_POSTPONED_JOB_INTERRUPT(get_valid_ec(vm));
}


static int
pjob_register_legacy_impl(unsigned int flags, rb_postponed_job_func_t func, void *data)
{
    /* We _know_ calling preregister from a signal handler like this is racy; what is
     * and is not promised is very exhaustively documented in debug.h */
    rb_postponed_job_handle_t h = rb_postponed_job_preregister(0, func, data);
    if (h == POSTPONED_JOB_HANDLE_INVALID) {
        return 0;
    }
    rb_postponed_job_trigger(h);
    return 1;
}

int
rb_postponed_job_register(unsigned int flags, rb_postponed_job_func_t func, void *data)
{
    return pjob_register_legacy_impl(flags, func, data);
}

int
rb_postponed_job_register_one(unsigned int flags, rb_postponed_job_func_t func, void *data)
{
    return pjob_register_legacy_impl(flags, func, data);
}


void
rb_postponed_job_flush(rb_vm_t *vm)
{
    rb_postponed_job_queues_t *pjq = GET_VM()->postponed_job_queue;
    rb_execution_context_t *ec = GET_EC();
    const rb_atomic_t block_mask = POSTPONED_JOB_INTERRUPT_MASK | TRAP_INTERRUPT_MASK;
    volatile rb_atomic_t saved_mask = ec->interrupt_mask & block_mask;
    VALUE volatile saved_errno = ec->errinfo;
    struct ccan_list_head tmp;

    ccan_list_head_init(&tmp);

    rb_nativethread_lock_lock(&vm->workqueue_lock);
    ccan_list_append_list(&tmp, &vm->workqueue);
    rb_nativethread_lock_unlock(&vm->workqueue_lock);

    rb_atomic_t triggered_bits = RUBY_ATOMIC_EXCHANGE(pjq->triggered_bitset, 0);

    ec->errinfo = Qnil;
    /* mask POSTPONED_JOB dispatch */
    ec->interrupt_mask |= block_mask;
    {
        EC_PUSH_TAG(ec);
        if (EC_EXEC_TAG() == TAG_NONE) {
            /* execute postponed jobs */
            while (triggered_bits) {
                unsigned int i = bit_length(triggered_bits) - 1;
                triggered_bits ^= ((1UL) << i); /* toggle ith bit off */
                rb_postponed_job_func_t func = pjq->table[i].func;
                void *data = pjq->table[i].data;
                (func)(data);
            }

            /* execute workqueue jobs */
            struct rb_workqueue_job *wq_job;
            while ((wq_job = ccan_list_pop(&tmp, struct rb_workqueue_job, jnode))) {
                rb_postponed_job_func_t func = wq_job->func;
                void *data = wq_job->data;

                free(wq_job);
                (func)(data);
            }
        }
        EC_POP_TAG();
    }
    /* restore POSTPONED_JOB mask */
    ec->interrupt_mask &= ~(saved_mask ^ block_mask);
    ec->errinfo = saved_errno;

    /* If we threw an exception, there might be leftover workqueue items; carry them over
     * to a subsequent execution of flush */
    if (!ccan_list_empty(&tmp)) {
        rb_nativethread_lock_lock(&vm->workqueue_lock);
        ccan_list_prepend_list(&vm->workqueue, &tmp);
        rb_nativethread_lock_unlock(&vm->workqueue_lock);

        RUBY_VM_SET_POSTPONED_JOB_INTERRUPT(GET_EC());
    }
    /* likewise with any remaining-to-be-executed bits of the preregistered postponed
     * job table */
    if (triggered_bits) {
        RUBY_ATOMIC_OR(pjq->triggered_bitset, triggered_bits);
        RUBY_VM_SET_POSTPONED_JOB_INTERRUPT(GET_EC());
    }
}
