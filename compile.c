#ifndef _GNU_SOURCE
#define _GNU_SOURCE // for strdup
#endif
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "compile.h"
#include "bytecode.h"
#include "locfile.h"
#include "jv_alloc.h"
#include "linker.h"

/*
  The intermediate representation for jq filters is as a sequence of
  struct inst, which form a doubly-linked list via the next and prev
  pointers. 

  A "block" represents a sequence of "struct inst", which may be
  empty.

  Blocks are generated by the parser bottom-up, so may have free
  variables (refer to things not defined). See inst.bound_by and
  inst.symbol.
 */
struct inst {
  struct inst* next;
  struct inst* prev;

  opcode op;
  
  struct {
    uint16_t intval;
    struct inst* target;
    jv constant;
    const struct cfunction* cfunc;
  } imm;

  struct locfile* locfile;
  location source;

  // Binding
  // An instruction requiring binding (for parameters/variables/functions)
  // is in one of three states:
  //   inst->bound_by = NULL  - Unbound free variable
  //   inst->bound_by = inst  - This instruction binds a variable
  //   inst->bound_by = other - Uses variable bound by other instruction
  // Unbound instructions (references to other things that may or may not 
  // exist) are created by "gen_foo_unbound", and bindings are created by
  // block_bind(definition, body), which binds all instructions in
  // body which are unboudn and refer to "definition" by name.
  struct inst* bound_by;
  char* symbol;

  int nformals;
  int nactuals;

  block subfn;   // used by CLOSURE_CREATE (body of function)
  block arglist; // used by CLOSURE_CREATE (formals) and CALL_JQ (arguments)

  // This instruction is compiled as part of which function?
  // (only used during block_compile)
  struct bytecode* compiled;

  int bytecode_pos; // position just after this insn
};

static inst* inst_new(opcode op) {
  inst* i = jv_mem_alloc(sizeof(inst));
  i->next = i->prev = 0;
  i->op = op;
  i->bytecode_pos = -1;
  i->bound_by = 0;
  i->symbol = 0;
  i->nformals = -1;
  i->nactuals = -1;
  i->subfn = gen_noop();
  i->arglist = gen_noop();
  i->source = UNKNOWN_LOCATION;
  i->locfile = 0;
  return i;
}

static void inst_free(struct inst* i) {
  jv_mem_free(i->symbol);
  block_free(i->subfn);
  block_free(i->arglist);
  if (i->locfile)
    locfile_free(i->locfile);
  if (opcode_describe(i->op)->flags & OP_HAS_CONSTANT) {
    jv_free(i->imm.constant);
  }
  jv_mem_free(i);
}

static block inst_block(inst* i) {
  block b = {i,i};
  return b;
}

int block_is_single(block b) {
  return b.first && b.first == b.last;
}

static inst* block_take(block* b) {
  if (b->first == 0) return 0;
  inst* i = b->first;
  if (i->next) {
    i->next->prev = 0;
    b->first = i->next;
    i->next = 0;
  } else {
    b->first = 0;
    b->last = 0;
  }
  return i;
}

block gen_location(location loc, struct locfile* l, block b) {
  for (inst* i = b.first; i; i = i->next) {
    if (i->source.start == UNKNOWN_LOCATION.start &&
        i->source.end == UNKNOWN_LOCATION.end) {
      i->source = loc;
      i->locfile = locfile_retain(l);
    }
  }
  return b;
}

block gen_noop() {
  block b = {0,0};
  return b;
}

int block_is_noop(block b) {
  return (b.first == 0 && b.last == 0);
}

block gen_op_simple(opcode op) {
  assert(opcode_describe(op)->length == 1);
  return inst_block(inst_new(op));
}


block gen_const(jv constant) {
  assert(opcode_describe(LOADK)->flags & OP_HAS_CONSTANT);
  inst* i = inst_new(LOADK);
  i->imm.constant = constant;
  return inst_block(i);
}

int block_is_const(block b) {
  return (block_is_single(b) && b.first->op == LOADK);
}

jv_kind block_const_kind(block b) {
  assert(block_is_const(b));
  return jv_get_kind(b.first->imm.constant);
}

jv block_const(block b) {
  assert(block_is_const(b));
  return jv_copy(b.first->imm.constant);
}

block gen_op_target(opcode op, block target) {
  assert(opcode_describe(op)->flags & OP_HAS_BRANCH);
  assert(target.last);
  inst* i = inst_new(op);
  i->imm.target = target.last;
  return inst_block(i);
}

block gen_op_targetlater(opcode op) {
  assert(opcode_describe(op)->flags & OP_HAS_BRANCH);
  inst* i = inst_new(op);
  i->imm.target = 0;
  return inst_block(i);
}
void inst_set_target(block b, block target) {
  assert(block_is_single(b));
  assert(opcode_describe(b.first->op)->flags & OP_HAS_BRANCH);
  assert(target.last);
  b.first->imm.target = target.last;
}

block gen_op_unbound(opcode op, const char* name) {
  assert(opcode_describe(op)->flags & OP_HAS_BINDING);
  inst* i = inst_new(op);
  i->symbol = strdup(name);
  return inst_block(i);
}

block gen_op_var_fresh(opcode op, const char* name) {
  assert(opcode_describe(op)->flags & OP_HAS_VARIABLE);
  return block_bind(gen_op_unbound(op, name),
                    gen_noop(), OP_HAS_VARIABLE);
}

block gen_op_bound(opcode op, block binder) {
  assert(block_is_single(binder));
  block b = gen_op_unbound(op, binder.first->symbol);
  b.first->bound_by = binder.first;
  return b;
}


static void inst_join(inst* a, inst* b) {
  assert(a && b);
  assert(!a->next);
  assert(!b->prev);
  a->next = b;
  b->prev = a;
}

void block_append(block* b, block b2) {
  if (b2.first) {
    if (b->last) {
      inst_join(b->last, b2.first);
    } else {
      b->first = b2.first;
    }
    b->last = b2.last;
  }
}

block block_join(block a, block b) {
  block c = a;
  block_append(&c, b);
  return c;
}

int block_has_only_binders_and_imports(block binders, int bindflags) {
  bindflags |= OP_HAS_BINDING;
  for (inst* curr = binders.first; curr; curr = curr->next) {
    if ((opcode_describe(curr->op)->flags & bindflags) != bindflags && curr->op != DEPS) {
      return 0;
    }
  }
  return 1;
}

int block_has_only_binders(block binders, int bindflags) {
  bindflags |= OP_HAS_BINDING;
  for (inst* curr = binders.first; curr; curr = curr->next) {
    if ((opcode_describe(curr->op)->flags & bindflags) != bindflags) {
      return 0;
    }
  }
  return 1;
}

// Count a binder's (function) formal params
static int block_count_formals(block b) {
  int args = 0;
  if (b.first->op == CLOSURE_CREATE_C)
    return b.first->imm.cfunc->nargs - 1;
  for (inst* i = b.first->arglist.first; i; i = i->next) {
    assert(i->op == CLOSURE_PARAM);
    args++;
  }
  return args;
}

// Count a call site's actual params
static int block_count_actuals(block b) {
  int args = 0;
  for (inst* i = b.first; i; i = i->next) {
    switch (i->op) {
    default: assert(0 && "Unknown function type"); break;
    case CLOSURE_CREATE: 
    case CLOSURE_PARAM:
    case CLOSURE_CREATE_C:
      args++;
      break;
    }
  }
  return args;
}

static int block_count_refs(block binder, block body) {
  int nrefs = 0;
  for (inst* i = body.first; i; i = i->next) {
    if (i != binder.first && i->bound_by == binder.first) {
      nrefs++;
    }
    // counting recurses into closures
    nrefs += block_count_refs(binder, i->subfn);
    // counting recurses into argument list
    nrefs += block_count_refs(binder, i->arglist);
  }
  return nrefs;
}

static int block_bind_subblock(block binder, block body, int bindflags) {
  assert(block_is_single(binder));
  assert((opcode_describe(binder.first->op)->flags & bindflags) == bindflags);
  assert(binder.first->symbol);
  assert(binder.first->bound_by == 0 || binder.first->bound_by == binder.first);

  binder.first->bound_by = binder.first;
  if (binder.first->nformals == -1)
    binder.first->nformals = block_count_formals(binder);
  int nrefs = 0;
  for (inst* i = body.first; i; i = i->next) {
    int flags = opcode_describe(i->op)->flags;
    if ((flags & bindflags) == bindflags &&
        i->bound_by == 0 &&
        !strcmp(i->symbol, binder.first->symbol)) {
      // bind this instruction
      if (i->op == CALL_JQ && i->nactuals == -1)
        i->nactuals = block_count_actuals(i->arglist);
      if (i->nactuals == -1 || i->nactuals == binder.first->nformals) {
        i->bound_by = binder.first;
        nrefs++;
      }
    }
    // binding recurses into closures
    nrefs += block_bind_subblock(binder, i->subfn, bindflags);
    // binding recurses into argument list
    nrefs += block_bind_subblock(binder, i->arglist, bindflags);
  }
  return nrefs;
}

static int block_bind_each(block binder, block body, int bindflags) {
  assert(block_has_only_binders(binder, bindflags));
  bindflags |= OP_HAS_BINDING;
  int nrefs = 0;
  for (inst* curr = binder.first; curr; curr = curr->next) {
    nrefs += block_bind_subblock(inst_block(curr), body, bindflags);
  }
  return nrefs;
}

block block_bind(block binder, block body, int bindflags) {
  block_bind_each(binder, body, bindflags);
  return block_join(binder, body);
}

block block_bind_library(block binder, block body, int bindflags, const char* libname) {
  assert(block_has_only_binders(binder, bindflags));
  bindflags |= OP_HAS_BINDING;
  int nrefs = 0;
  int matchlen = strlen(libname)+2;
  char* matchname = malloc(matchlen+1);
  strcpy(matchname,libname);
  strcpy(matchname+matchlen-2,"::");
  for (inst *curr = binder.first; curr; curr = curr->next) {
    char* cname = curr->symbol;
    char* tname = malloc(strlen(curr->symbol)+matchlen+1);
    strcpy(tname, matchname);
    strcpy(tname+matchlen,cname);
    curr->symbol = tname;
    nrefs += block_bind_subblock(inst_block(curr), body, bindflags);
    curr->symbol = cname;
    free(tname);
  }
  free(matchname);
  return body; // We don't return a join because we don't want those sticking around...
}

// Bind binder to body and throw away any defs in binder not referenced
// (directly or indirectly) from body.
block block_bind_referenced(block binder, block body, int bindflags) {
  assert(block_has_only_binders(binder, bindflags));
  bindflags |= OP_HAS_BINDING;
  block refd = gen_noop();
  block unrefd = gen_noop();
  int nrefs;
  for (int last_kept = 0, kept = 0; ; ) {
    for (inst* curr; (curr = block_take(&binder));) {
      block b = inst_block(curr);
      nrefs = block_bind_each(b, body, bindflags);
      // Check if this binder is referenced from any of the ones we
      // already know are referenced by body.
      nrefs += block_count_refs(b, refd);
      nrefs += block_count_refs(b, body);
      if (nrefs) {
        refd = BLOCK(refd, b);
        kept++;
      } else {
        unrefd = BLOCK(unrefd, b);
      }
    }
    if (kept == last_kept)
      break;
    last_kept = kept;
    binder = unrefd;
    unrefd = gen_noop();
  }
  block_free(unrefd);
  return block_join(refd, body);
}

block block_drop_unreferenced(block body) {
  inst* curr;
  block refd = gen_noop();
  block unrefd = gen_noop();
  int drop;
  do {
    drop = 0;
    while((curr = block_take(&body)) && curr->op != TOP) {
      block b = inst_block(curr);
      if (block_count_refs(b,refd) + block_count_refs(b,body) == 0) {
        unrefd = BLOCK(unrefd, b);
        drop++;
      } else {
        refd = BLOCK(refd, b);
      }
    }
    if (curr && curr->op == TOP) {
      body = BLOCK(inst_block(curr),body);
    }
    body = BLOCK(refd, body);
    refd = gen_noop();
  } while (drop != 0);
  block_free(unrefd);
  return body;
}

jv block_take_imports(block* body) {
  jv imports = jv_array();
  
  inst* top = NULL;
  if (body->first->op == TOP) {
    top = block_take(body);
  }
  while (body->first && body->first->op == DEPS) {
    inst* dep = block_take(body);
    jv opts = jv_copy(dep->imm.constant);
    opts = jv_object_set(opts,jv_string("name"),jv_string(dep->symbol));
    imports = jv_array_append(imports, opts);
    inst_free(dep);
  }
  if (top) {
    *body = block_join(inst_block(top),*body);
  }
  return imports;
}

block gen_import(const char* name, const char* as, const char* search) {
  inst* i = inst_new(DEPS);
  i->symbol = strdup(name);
  jv opts = jv_object();
  if (as)
	  opts = jv_object_set(opts, jv_string("as"), jv_string(as));
  if (search)
	  opts = jv_object_set(opts, jv_string("search"), jv_string(search));
  i->imm.constant = opts;
  return inst_block(i);
}

block gen_function(const char* name, block formals, block body) {
  block_bind_each(formals, body, OP_IS_CALL_PSEUDO);
  inst* i = inst_new(CLOSURE_CREATE);
  i->subfn = body;
  i->symbol = strdup(name);
  i->arglist = formals;
  block b = inst_block(i);
  block_bind_subblock(b, b, OP_IS_CALL_PSEUDO | OP_HAS_BINDING);
  return b;
}

block gen_param(const char* name) {
  return gen_op_unbound(CLOSURE_PARAM, name);
}

block gen_lambda(block body) {
  return gen_function("@lambda", gen_noop(), body);
}

block gen_call(const char* name, block args) {
  block b = gen_op_unbound(CALL_JQ, name);
  b.first->arglist = args;
  return b;
}



block gen_subexp(block a) {
  return BLOCK(gen_op_simple(SUBEXP_BEGIN), a, gen_op_simple(SUBEXP_END));
}

block gen_both(block a, block b) {
  block jump = gen_op_targetlater(JUMP);
  block fork = gen_op_target(FORK, jump);
  block c = BLOCK(fork, a, jump, b);
  inst_set_target(jump, c);
  return c;
}


block gen_collect(block expr) {
  block array_var = gen_op_var_fresh(STOREV, "collect");
  block c = BLOCK(gen_op_simple(DUP), gen_const(jv_array()), array_var);

  block tail = BLOCK(gen_op_bound(APPEND, array_var),
                     gen_op_simple(BACKTRACK));

  return BLOCK(c,
               gen_op_target(FORK, tail),
               expr, 
               tail,
               gen_op_bound(LOADVN, array_var));
}

block gen_reduce(const char* varname, block source, block init, block body) {
  block res_var = gen_op_var_fresh(STOREV, "reduce");
  block loop = BLOCK(gen_op_simple(DUP),
                     source,
                     block_bind(gen_op_unbound(STOREV, varname),
                                BLOCK(gen_op_bound(LOADVN, res_var),
                                      body,
                                      gen_op_bound(STOREV, res_var)),
                                OP_HAS_VARIABLE),
                     gen_op_simple(BACKTRACK));
  return BLOCK(gen_op_simple(DUP),
               init,
               res_var,
               gen_op_target(FORK, loop),
               loop,
               gen_op_bound(LOADVN, res_var));
}

block gen_foreach(const char* varname, block source, block init, block update, block extract) {
  block output = gen_op_targetlater(JUMP);
  block state_var = gen_op_var_fresh(STOREV, "foreach");
  block loop = BLOCK(gen_op_simple(DUP),
                     // get a value from the source expression:
                     source,
                     // bind the $varname to that value for all the code in
                     // this block_bind() to see:
                     block_bind(gen_op_unbound(STOREV, varname),
                                // load the loop state variable
                                BLOCK(gen_op_bound(LOADVN, state_var),
                                      // generate updated state
                                      update,
                                      // save the updated state for value extraction
                                      gen_op_simple(DUP),
                                      // save new state
                                      gen_op_bound(STOREV, state_var),
                                      // extract an output...
                                      extract,
                                      // ...and output it
                                      output),
                                OP_HAS_VARIABLE));
  block foreach = BLOCK(gen_op_simple(DUP),
                        init,
                        state_var,
                        gen_op_target(FORK, loop),
                        loop,
                        // At this point `foreach`'s input will be on
                        // top of the stack, and we don't want to output
                        // it, so we backtrack.
                        gen_op_simple(BACKTRACK));
  inst_set_target(output, foreach);
  block handler = gen_cond(gen_call("_equal", BLOCK(gen_lambda(gen_const(jv_string("break"))), gen_lambda(gen_noop()))),
                           gen_op_simple(BACKTRACK),
                           gen_call("break", gen_noop()));
  return gen_try(foreach, handler);
}

block gen_definedor(block a, block b) {
  // var found := false
  block found_var = gen_op_var_fresh(STOREV, "found");
  block init = BLOCK(gen_op_simple(DUP), gen_const(jv_false()), found_var);

  // if found, backtrack. Otherwise execute b
  block backtrack = gen_op_simple(BACKTRACK);
  block tail = BLOCK(gen_op_simple(DUP),
                     gen_op_bound(LOADV, found_var),
                     gen_op_target(JUMP_F, backtrack),
                     backtrack,
                     gen_op_simple(POP),
                     b);

  // try again
  block if_notfound = gen_op_simple(BACKTRACK);

  // found := true, produce result
  block if_found = BLOCK(gen_op_simple(DUP),
                         gen_const(jv_true()),
                         gen_op_bound(STOREV, found_var),
                         gen_op_target(JUMP, tail));

  return BLOCK(init,
               gen_op_target(FORK, if_notfound),
               a,
               gen_op_target(JUMP_F, if_found),
               if_found,
               if_notfound,
               tail);
}

int block_has_main(block top) {
  return top.first && top.first->op == TOP;
}

int block_is_funcdef(block b) {
  if (b.first != NULL && b.first->op == CLOSURE_CREATE)
    return 1;
  return 0;
}

block gen_condbranch(block iftrue, block iffalse) {
  iftrue = BLOCK(iftrue, gen_op_target(JUMP, iffalse));
  return BLOCK(gen_op_target(JUMP_F, iftrue), iftrue, iffalse);
}

block gen_and(block a, block b) {
  // a and b = if a then (if b then true else false) else false
  return BLOCK(gen_op_simple(DUP), a, 
               gen_condbranch(BLOCK(gen_op_simple(POP),
                                    b,
                                    gen_condbranch(gen_const(jv_true()),
                                                   gen_const(jv_false()))),
                              BLOCK(gen_op_simple(POP), gen_const(jv_false()))));
}

block gen_or(block a, block b) {
  // a or b = if a then true else (if b then true else false)
  return BLOCK(gen_op_simple(DUP), a,
               gen_condbranch(BLOCK(gen_op_simple(POP), gen_const(jv_true())),
                              BLOCK(gen_op_simple(POP),
                                    b,
                                    gen_condbranch(gen_const(jv_true()),
                                                   gen_const(jv_false())))));
}

block gen_var_binding(block var, const char* name, block body) {
  return BLOCK(gen_op_simple(DUP), var,
               block_bind(gen_op_unbound(STOREV, name),
                          body, OP_HAS_VARIABLE));
}

block gen_cond(block cond, block iftrue, block iffalse) {
  return BLOCK(gen_op_simple(DUP), cond, 
               gen_condbranch(BLOCK(gen_op_simple(POP), iftrue),
                              BLOCK(gen_op_simple(POP), iffalse)));
}

block gen_try(block exp, block handler) {
  /*
   * Produce:
   *  FORK_OPT <address of handler>
   *  <exp>
   *  JUMP <end of handler>
   *  <handler>
   *
   * The handler will only execute if we backtrack to the FORK_OPT with
   * an error (exception).  If <exp> produces no value then FORK_OPT
   * will backtrack (propagate the `empty`, as it were.  If <exp>
   * produces a value then we'll execute whatever bytecode follows this
   * sequence.
   */
  if (!handler.first && !handler.last)
    // A hack to deal with `.` as the handler; we could use a real NOOP here
    handler = BLOCK(gen_op_simple(DUP), gen_op_simple(POP), handler);
  exp = BLOCK(exp, gen_op_target(JUMP, handler));
  return BLOCK(gen_op_target(FORK_OPT, exp), exp, handler);
}

block gen_cbinding(const struct cfunction* cfunctions, int ncfunctions, block code) {
  for (int cfunc=0; cfunc<ncfunctions; cfunc++) {
    inst* i = inst_new(CLOSURE_CREATE_C);
    i->imm.cfunc = &cfunctions[cfunc];
    i->symbol = strdup(i->imm.cfunc->name);
    code = block_bind(inst_block(i), code, OP_IS_CALL_PSEUDO);
  }
  return code;
}

static uint16_t nesting_level(struct bytecode* bc, inst* target) {
  uint16_t level = 0;
  assert(bc && target->compiled);
  while (bc && target->compiled != bc) {
    level++;
    bc = bc->parent;
  }
  assert(bc && bc == target->compiled);
  return level;
}

static int count_cfunctions(block b) {
  int n = 0;
  for (inst* i = b.first; i; i = i->next) {
    if (i->op == CLOSURE_CREATE_C) n++;
    n += count_cfunctions(i->subfn);
  }
  return n;
}


// Expands call instructions into a calling sequence
static int expand_call_arglist(block* b) {
  int errors = 0;
  block ret = gen_noop();
  for (inst* curr; (curr = block_take(b));) {
    if (opcode_describe(curr->op)->flags & OP_HAS_BINDING) {
      if (!curr->bound_by) {
        locfile_locate(curr->locfile, curr->source, "error: %s/%d is not defined", curr->symbol, block_count_actuals(curr->arglist));
        errors++;
        // don't process this instruction if it's not well-defined
        ret = BLOCK(ret, inst_block(curr));
        continue;
      }
    }

    block prelude = gen_noop();
    if (curr->op == CALL_JQ) {
      int actual_args = 0, desired_args = 0;
      // We expand the argument list as a series of instructions
      switch (curr->bound_by->op) {
      default: assert(0 && "Unknown function type"); break;
      case CLOSURE_CREATE: 
      case CLOSURE_PARAM: {
        block callargs = gen_noop();
        for (inst* i; (i = block_take(&curr->arglist));) {
          assert(opcode_describe(i->op)->flags & OP_IS_CALL_PSEUDO);
          block b = inst_block(i);
          switch (i->op) {
          default: assert(0 && "Unknown type of parameter"); break;
          case CLOSURE_REF:
            block_append(&callargs, b);
            break;
          case CLOSURE_CREATE:
            block_append(&prelude, b);
            block_append(&callargs, gen_op_bound(CLOSURE_REF, b));
            break;
          }
          actual_args++;
        }
        curr->imm.intval = actual_args;
        curr->arglist = callargs;

        if (curr->bound_by->op == CLOSURE_CREATE) {
          for (inst* i = curr->bound_by->arglist.first; i; i = i->next) {
            assert(i->op == CLOSURE_PARAM);
            desired_args++;
          }
        }
        break;
      }

      case CLOSURE_CREATE_C: {
        for (inst* i; (i = block_take(&curr->arglist)); ) {
          assert(i->op == CLOSURE_CREATE); // FIXME
          block body = i->subfn;
          i->subfn = gen_noop();
          inst_free(i);
          // arguments should be pushed in reverse order, prepend them to prelude
          errors += expand_call_arglist(&body);
          prelude = BLOCK(gen_subexp(body), prelude);
          actual_args++;
        }
        assert(curr->op == CALL_JQ);
        curr->op = CALL_BUILTIN;
        curr->imm.intval = actual_args + 1 /* include the implicit input in arg count */;
        assert(curr->bound_by->op == CLOSURE_CREATE_C);
        desired_args = curr->bound_by->imm.cfunc->nargs - 1;
        assert(!curr->arglist.first);
        break;
      }
      }

      assert(actual_args == desired_args); // because now handle this above
    }
    ret = BLOCK(ret, prelude, inst_block(curr));
  }
  *b = ret;
  return errors;
}

static int compile(struct bytecode* bc, block b) {
  int errors = 0;
  int pos = 0;
  int var_frame_idx = 0;
  bc->nsubfunctions = 0;
  errors += expand_call_arglist(&b);
  b = BLOCK(b, gen_op_simple(RET));
  jv localnames = jv_array();
  for (inst* curr = b.first; curr; curr = curr->next) {
    if (!curr->next) assert(curr == b.last);
    int length = opcode_describe(curr->op)->length;
    if (curr->op == CALL_JQ) {
      for (inst* arg = curr->arglist.first; arg; arg = arg->next) {
        length += 2;
      }
    }
    pos += length;
    curr->bytecode_pos = pos;
    curr->compiled = bc;

    assert(curr->op != CLOSURE_REF && curr->op != CLOSURE_PARAM);

    if ((opcode_describe(curr->op)->flags & OP_HAS_VARIABLE) &&
        curr->bound_by == curr) {
      curr->imm.intval = var_frame_idx++;
      localnames = jv_array_append(localnames, jv_string(curr->symbol));
    }

    if (curr->op == CLOSURE_CREATE) {
      assert(curr->bound_by == curr);
      curr->imm.intval = bc->nsubfunctions++;
    }
    if (curr->op == CLOSURE_CREATE_C) {
      assert(curr->bound_by == curr);
      int idx = bc->globals->ncfunctions++;
      bc->globals->cfunc_names = jv_array_append(bc->globals->cfunc_names,
                                                 jv_string(curr->symbol));
      bc->globals->cfunctions[idx] = *curr->imm.cfunc;
      curr->imm.intval = idx;
    }
  }
  bc->debuginfo = jv_object_set(bc->debuginfo, jv_string("locals"), localnames);
  if (bc->nsubfunctions) {
    bc->subfunctions = jv_mem_alloc(sizeof(struct bytecode*) * bc->nsubfunctions);
    for (inst* curr = b.first; curr; curr = curr->next) {
      if (curr->op == CLOSURE_CREATE) {
        struct bytecode* subfn = jv_mem_alloc(sizeof(struct bytecode));
        bc->subfunctions[curr->imm.intval] = subfn;
        subfn->globals = bc->globals;
        subfn->parent = bc;
        subfn->nclosures = 0;
        subfn->debuginfo = jv_object_set(jv_object(), jv_string("name"), jv_string(curr->symbol));
        jv params = jv_array();
        for (inst* param = curr->arglist.first; param; param = param->next) {
          assert(param->op == CLOSURE_PARAM);
          assert(param->bound_by == param);
          param->imm.intval = subfn->nclosures++;
          param->compiled = subfn;
          params = jv_array_append(params, jv_string(param->symbol));
        }
        subfn->debuginfo = jv_object_set(subfn->debuginfo, jv_string("params"), params);
        errors += compile(subfn, curr->subfn);
        curr->subfn = gen_noop();
      }
    }
  } else {
    bc->subfunctions = 0;
  }
  bc->codelen = pos;
  uint16_t* code = jv_mem_alloc(sizeof(uint16_t) * bc->codelen);
  bc->code = code;
  pos = 0;
  jv constant_pool = jv_array();
  int maxvar = -1;
  if (!errors) for (inst* curr = b.first; curr; curr = curr->next) {
    const struct opcode_description* op = opcode_describe(curr->op);
    if (op->length == 0)
      continue;
    code[pos++] = curr->op;
    assert(curr->op != CLOSURE_REF && curr->op != CLOSURE_PARAM);
    if (curr->op == CALL_BUILTIN) {
      assert(curr->bound_by->op == CLOSURE_CREATE_C);
      assert(!curr->arglist.first);
      code[pos++] = (uint16_t)curr->imm.intval;
      code[pos++] = curr->bound_by->imm.intval;
    } else if (curr->op == CALL_JQ) {
      assert(curr->bound_by->op == CLOSURE_CREATE ||
             curr->bound_by->op == CLOSURE_PARAM);
      code[pos++] = (uint16_t)curr->imm.intval;
      code[pos++] = nesting_level(bc, curr->bound_by);
      code[pos++] = curr->bound_by->imm.intval | 
        (curr->bound_by->op == CLOSURE_CREATE ? ARG_NEWCLOSURE : 0);
      for (inst* arg = curr->arglist.first; arg; arg = arg->next) {
        assert(arg->op == CLOSURE_REF && arg->bound_by->op == CLOSURE_CREATE);
        code[pos++] = nesting_level(bc, arg->bound_by);
        code[pos++] = arg->bound_by->imm.intval | ARG_NEWCLOSURE;
      }
    } else if (op->flags & OP_HAS_CONSTANT) {
      code[pos++] = jv_array_length(jv_copy(constant_pool));
      constant_pool = jv_array_append(constant_pool, jv_copy(curr->imm.constant));
    } else if (op->flags & OP_HAS_VARIABLE) {
      code[pos++] = nesting_level(bc, curr->bound_by);
      uint16_t var = (uint16_t)curr->bound_by->imm.intval;
      code[pos++] = var;
      if (var > maxvar) maxvar = var;
    } else if (op->flags & OP_HAS_BRANCH) {
      assert(curr->imm.target->bytecode_pos != -1);
      assert(curr->imm.target->bytecode_pos > pos); // only forward branches
      code[pos] = curr->imm.target->bytecode_pos - (pos + 1);
      pos++;
    } else if (op->length > 1) {
      assert(0 && "codegen not implemented for this operation");
    }
  }
  bc->constants = constant_pool;
  bc->nlocals = maxvar + 2; // FIXME: frames of size zero?
  block_free(b);
  return errors;
}

int block_compile(block b, struct bytecode** out) {
  struct bytecode* bc = jv_mem_alloc(sizeof(struct bytecode));
  bc->parent = 0;
  bc->nclosures = 0;
  bc->globals = jv_mem_alloc(sizeof(struct symbol_table));
  int ncfunc = count_cfunctions(b);
  bc->globals->ncfunctions = 0;
  bc->globals->cfunctions = jv_mem_alloc(sizeof(struct cfunction) * ncfunc);
  bc->globals->cfunc_names = jv_array();
  bc->debuginfo = jv_object_set(jv_object(), jv_string("name"), jv_null());
  int nerrors = compile(bc, b);
  assert(bc->globals->ncfunctions == ncfunc);
  if (nerrors > 0) {
    bytecode_free(bc);
    *out = 0;
  } else {
    *out = bc;
  }
  return nerrors;
}

void block_free(block b) {
  struct inst* next;
  for (struct inst* curr = b.first; curr; curr = next) {
    next = curr->next;
    inst_free(curr);
  }
}
