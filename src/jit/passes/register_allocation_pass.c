#include <limits.h>
#include "jit/passes/register_allocation_pass.h"
#include "core/list.h"
#include "core/math.h"
#include "jit/backend/jit_backend.h"
#include "jit/ir/ir.h"
#include "jit/pass_stats.h"

/* second-chance binpacking register allocator based off of the paper "Quality
   and Speed in Linear-scan Register Allocation" by Omri Traub, Glenn Holloway
   and Michael D. Smith */

DEFINE_STAT(gprs_spilled, "gprs spilled");
DEFINE_STAT(fprs_spilled, "fprs spilled");

struct ra_tmp;

/* bins represent a single machine register into which temporaries are packed.
   the constraint on a bin is that it may contain only one valid temporary at
   any given time */
struct ra_bin {
  /* machine register backing this bin */
  const struct jit_register *reg;

  /* current temporary packed in this bin */
  int tmp_idx;
};

/* tmps represent a register allocation candidate

   on start, a temporary is created for each instruction result. the temporary
   is assigned the result's ir_value as its original location. however, the
   temporary may end up living in multiple locations during its lifetime

   when register pressure is high, a temporary may be spilled to the stack, at
   which point its value becomes NULL, and the slot becomes non-NULL

   before the temporary's next use, a fill back from the stack is inserted,
   producing a new non-NULL value to allocate for, but not touching the stack
   slot. slots are not reused by different temporaries, so once it has spilled
   once, it should not be spilled again */
struct ra_tmp {
  int next_use_idx;
  int last_use_idx;

  /* current location of temporary */
  struct ir_value *value;
  struct ir_local *slot;
};

/* uses represent a use of a temporary by an instruction */
struct ra_use {
  /* ordinal of instruction using the temporary */
  int ordinal;

  /* next use index */
  int next_idx;
};

/* register allocation works over extended basic blocks by recursively iterating
   each path in the control flow tree, passing the output state of each block as
   the input state of each of its successor blocks. all state that is passed in
   this way is encapsulated inside of the ra_state struct, which is pushed and
   popped to a stack while iterating through ra_push_state / ra_pop_state */
struct ra_state {
  struct ra_bin *bins;

  struct ra_tmp *tmps;
  int num_tmps;
  int max_tmps;

  struct list_node it;
};

struct ra {
  const struct jit_register *regs;
  int num_regs;

  /* uses are constant throughout allocation, no need to push / pop as part of
     ra_state struct */
  struct ra_use *uses;
  int num_uses;
  int max_uses;

  struct list live_state;
  struct list free_state;
  struct ra_state *state;
};

#define NO_TMP -1
#define NO_USE -1

#define ra_get_bin(i) &ra->state->bins[(i)]

#define ra_get_ordinal(i) ((int)(i)->tag)
#define ra_set_ordinal(i, ord) (i)->tag = (intptr_t)(ord)

#define ra_get_packed(b) \
  ((b)->tmp_idx == NO_TMP ? NULL : &ra->state->tmps[(b)->tmp_idx])
#define ra_set_packed(b, t) (b)->tmp_idx = (t) ? (t)-ra->state->tmps : NO_TMP

#define ra_get_tmp(v) (&ra->state->tmps[(v)->tag])
#define ra_set_tmp(v, t) (v)->tag = (t)-ra->state->tmps

static int ra_reg_can_store(const struct jit_register *reg,
                            const struct ir_value *v) {
  int mask = 1 << v->type;
  return (reg->value_types & mask) == mask;
}

static void ra_reset_state(struct ra *ra, struct ra_state *state) {
  for (int i = 0; i < ra->num_regs; i++) {
    struct ra_bin *bin = &state->bins[i];
    bin->tmp_idx = NO_TMP;
  }

  state->num_tmps = 0;
}

static void ra_copy_state(struct ra *ra, struct ra_state *dst,
                          struct ra_state *src) {
  if (src->num_tmps > dst->max_tmps) {
    dst->max_tmps = src->max_tmps;
    dst->tmps = realloc(dst->tmps, dst->max_tmps * sizeof(struct ra_tmp));
  }

  memcpy(dst->bins, src->bins, ra->num_regs * sizeof(struct ra_bin));
  memcpy(dst->tmps, src->tmps, src->num_tmps * sizeof(struct ra_tmp));
  dst->num_tmps = src->num_tmps;
}

static void ra_destroy_state(struct ra *ra, struct ra_state *state) {
  free(state->tmps);
  free(state->bins);
  free(state);
}

static struct ra_state *ra_create_state(struct ra *ra) {
  struct ra_state *state = calloc(1, sizeof(struct ra_state));

  state->bins = calloc(ra->num_regs, sizeof(struct ra_bin));

  for (int i = 0; i < ra->num_regs; i++) {
    struct ra_bin *bin = &state->bins[i];
    bin->reg = &ra->regs[i];
  }

  return state;
}

static void ra_pop_state(struct ra *ra) {
  /* pop top state from live list */
  struct ra_state *state =
      list_last_entry(&ra->live_state, struct ra_state, it);
  CHECK_NOTNULL(state);
  list_remove(&ra->live_state, &state->it);

  /* push state back to free list */
  list_add(&ra->free_state, &state->it);

  /* cache off latest state */
  state = list_last_entry(&ra->live_state, struct ra_state, it);

  ra->state = state;
}

static void ra_push_state(struct ra *ra) {
  struct ra_state *state =
      list_first_entry(&ra->free_state, struct ra_state, it);

  if (state) {
    /* remove from the free list */
    list_remove(&ra->free_state, &state->it);
  } else {
    /* allocate new state if one wasn't available on the free list */
    state = ra_create_state(ra);
  }

  /* push state to live list */
  list_add(&ra->live_state, &state->it);

  /* copy previous state into new state */
  if (ra->state) {
    ra_copy_state(ra, state, ra->state);
  } else {
    ra_reset_state(ra, state);
  }

  /* cache off latest state */
  ra->state = state;
}

static void ra_add_use(struct ra *ra, struct ra_tmp *tmp, int ordinal) {
  if (ra->num_uses >= ra->max_uses) {
    /* grow array */
    int old_max = ra->max_uses;
    ra->max_uses = MAX(32, ra->max_uses * 2);
    ra->uses = realloc(ra->uses, ra->max_uses * sizeof(struct ra_use));

    /* initialize the new entries */
    memset(ra->uses + old_max, 0,
           (ra->max_uses - old_max) * sizeof(struct ra_use));
  }

  struct ra_use *use = &ra->uses[ra->num_uses];
  use->ordinal = ordinal;
  use->next_idx = NO_USE;

  /* append use to temporary's list of uses */
  if (tmp->next_use_idx == NO_USE) {
    CHECK_EQ(tmp->last_use_idx, NO_USE);
    tmp->next_use_idx = ra->num_uses;
    tmp->last_use_idx = ra->num_uses;
  } else {
    CHECK_NE(tmp->last_use_idx, NO_USE);
    struct ra_use *last_use = &ra->uses[tmp->last_use_idx];
    last_use->next_idx = ra->num_uses;
    tmp->last_use_idx = ra->num_uses;
  }

  ra->num_uses++;
}

static struct ra_tmp *ra_create_tmp(struct ra *ra, struct ir_value *value) {
  if (ra->state->num_tmps >= ra->state->max_tmps) {
    /* grow array */
    int old_max = ra->state->max_tmps;
    ra->state->max_tmps = MAX(32, ra->state->max_tmps * 2);
    ra->state->tmps =
        realloc(ra->state->tmps, ra->state->max_tmps * sizeof(struct ra_tmp));

    /* initialize the new entries */
    memset(ra->state->tmps + old_max, 0,
           (ra->state->max_tmps - old_max) * sizeof(struct ra_tmp));
  }

  /* reset the temporary's state, reusing the previously allocated uses array */
  struct ra_tmp *tmp = &ra->state->tmps[ra->state->num_tmps];
  tmp->next_use_idx = NO_USE;
  tmp->last_use_idx = NO_USE;
  tmp->value = NULL;
  tmp->slot = NULL;

  /* assign the temporary to the value */
  value->tag = ra->state->num_tmps++;

  return tmp;
}

static void ra_validate_r(struct ra *ra, struct ir *ir, struct ir_block *block,
                          struct ir_value **active_in) {
  size_t active_size = sizeof(struct ir_value *) * ra->num_regs;
  struct ir_value **active = alloca(active_size);

  if (active_in) {
    memcpy(active, active_in, active_size);
  } else {
    memset(active, 0, active_size);
  }

  list_for_each_entry_safe(instr, &block->instrs, struct ir_instr, it) {
    for (int i = 0; i < MAX_INSTR_ARGS; i++) {
      struct ir_value *arg = instr->arg[i];

      if (!arg || ir_is_constant(arg)) {
        continue;
      }

      /* make sure the argument is the current value in the register */
      CHECK_EQ(active[arg->reg], arg);
    }

    if (instr->result) {
      active[instr->result->reg] = instr->result;
    }
  }

  list_for_each_entry(edge, &block->outgoing, struct ir_edge, it) {
    ra_validate_r(ra, ir, edge->dst, active);
  }
}

static void ra_validate(struct ra *ra, struct ir *ir) {
  struct ir_block *head_block =
      list_first_entry(&ir->blocks, struct ir_block, it);
  ra_validate_r(ra, ir, head_block, NULL);
}

static void ra_pack_bin(struct ra *ra, struct ra_bin *bin,
                        struct ra_tmp *new_tmp) {
  struct ra_tmp *old_tmp = ra_get_packed(bin);

  if (old_tmp) {
    /* the existing temporary is no longer available in the bin's register */
    old_tmp->value = NULL;
  }

  if (new_tmp) {
    /* assign the bin's register to the new temporary */
    int reg = (int)(bin->reg - ra->regs);
    new_tmp->value->reg = reg;
  }

  ra_set_packed(bin, new_tmp);
}

static int ra_alloc_blocked_reg(struct ra *ra, struct ir *ir,
                                struct ra_tmp *tmp) {
  /* find the register who's next use is furthest away */
  struct ra_bin *spill_bin = NULL;
  int furthest_use = INT_MIN;

  for (int i = 0; i < ra->num_regs; i++) {
    struct ra_bin *bin = ra_get_bin(i);
    struct ra_tmp *packed = ra_get_packed(bin);

    if (!packed) {
      continue;
    }

    if (!ra_reg_can_store(bin->reg, tmp->value)) {
      continue;
    }

    struct ra_use *next_use = &ra->uses[packed->next_use_idx];

    if (next_use->ordinal > furthest_use) {
      furthest_use = next_use->ordinal;
      spill_bin = bin;
    }
  }

  if (!spill_bin) {
    return 0;
  }

  /* spill the tmp if it wasn't previously spilled */
  struct ra_tmp *spill_tmp = ra_get_packed(spill_bin);

  if (!spill_tmp->slot) {
    struct ir_instr *spill_after =
        list_prev_entry(tmp->value->def, struct ir_instr, it);
    struct ir_insert_point point = {tmp->value->def->block, spill_after};
    ir_set_insert_point(ir, &point);

    spill_tmp->slot = ir_alloc_local(ir, spill_tmp->value->type);
    ir_store_local(ir, spill_tmp->slot, spill_tmp->value);

    /* track spill stats */
    if (ir_is_int(spill_tmp->value->type)) {
      STAT_gprs_spilled++;
    } else {
      STAT_fprs_spilled++;
    }
  }

  /* assign temporary to spilled value's bin */
  ra_pack_bin(ra, spill_bin, tmp);

  return 1;
}

static int ra_alloc_free_reg(struct ra *ra, struct ir *ir, struct ra_tmp *tmp) {
  /* find the first free register which can store the tmp's value */
  struct ra_bin *alloc_bin = NULL;

  for (int i = 0; i < ra->num_regs; i++) {
    struct ra_bin *bin = ra_get_bin(i);
    struct ra_tmp *packed = ra_get_packed(bin);

    if (packed) {
      continue;
    }

    if (!ra_reg_can_store(bin->reg, tmp->value)) {
      continue;
    }

    alloc_bin = bin;
    break;
  }

  if (!alloc_bin) {
    return 0;
  }

  /* assign the new tmp to the register's bin */
  ra_pack_bin(ra, alloc_bin, tmp);

  return 1;
}

static int ra_reuse_arg_reg(struct ra *ra, struct ir *ir, struct ra_tmp *tmp) {
  struct ir_instr *instr = tmp->value->def;
  int pos = ra_get_ordinal(instr);

  if (!instr->arg[0] || ir_is_constant(instr->arg[0])) {
    return 0;
  }

  /* if the argument's register is used after this instruction, it's not
     trivial to reuse */
  struct ra_tmp *arg = ra_get_tmp(instr->arg[0]);
  struct ra_use *next_use = &ra->uses[arg->next_use_idx];

  CHECK(arg->value && arg->value->reg != NO_REGISTER);

  if (next_use->next_idx != NO_USE) {
    return 0;
  }

  /* make sure the register can hold the tmp's value */
  struct ra_bin *reuse_bin = ra_get_bin(arg->value->reg);

  if (!ra_reg_can_store(reuse_bin->reg, tmp->value)) {
    return 0;
  }

  /* assign the new tmp to the register's bin */
  ra_pack_bin(ra, reuse_bin, tmp);

  return 1;
}

static void ra_alloc(struct ra *ra, struct ir *ir, struct ir_value *value) {
  if (!value) {
    return;
  }

  /* set initial value */
  struct ra_tmp *tmp = ra_get_tmp(value);
  tmp->value = value;

  if (!ra_reuse_arg_reg(ra, ir, tmp)) {
    if (!ra_alloc_free_reg(ra, ir, tmp)) {
      if (!ra_alloc_blocked_reg(ra, ir, tmp)) {
        LOG_FATAL("Failed to allocate register");
      }
    }
  }
}

static void ra_rewrite_arg(struct ra *ra, struct ir *ir, struct ir_instr *instr,
                           int n) {
  struct ir_use *use = &instr->used[n];
  struct ir_value *value = *use->parg;

  if (!value || ir_is_constant(value)) {
    return;
  }

  struct ra_tmp *tmp = ra_get_tmp(value);

  /* if the value isn't currently in a register, fill it from the stack */
  if (!tmp->value) {
    CHECK_NOTNULL(tmp->slot);

    struct ir_instr *fill_after = list_prev_entry(instr, struct ir_instr, it);
    struct ir_insert_point point = {instr->block, fill_after};
    ir_set_insert_point(ir, &point);

    struct ir_value *fill = ir_load_local(ir, tmp->slot);
    int ordinal = ra_get_ordinal(instr);
    ra_set_ordinal(fill->def, ordinal - MAX_INSTR_ARGS + n);
    fill->tag = value->tag;
    tmp->value = fill;

    ra_alloc(ra, ir, fill);
  }

  /* replace original value with the tmp's latest value */
  CHECK_NOTNULL(tmp->value);
  ir_replace_use(use, tmp->value);
}

static void ra_expire_tmps(struct ra *ra, struct ir *ir,
                           struct ir_instr *current) {
  int current_ordinal = ra_get_ordinal(current);

  /* free up any bins which contain tmps that have now expired */
  for (int i = 0; i < ra->num_regs; i++) {
    struct ra_bin *bin = ra_get_bin(i);
    struct ra_tmp *packed = ra_get_packed(bin);

    if (!packed) {
      continue;
    }

    while (1) {
      /* stop advancing once the next use is after the current position */
      struct ra_use *next_use = &ra->uses[packed->next_use_idx];

      if (next_use->ordinal >= current_ordinal) {
        break;
      }

      /* no more uses, expire temporary */
      if (next_use->next_idx == NO_USE) {
        ra_pack_bin(ra, bin, NULL);
        break;
      }

      packed->next_use_idx = next_use->next_idx;
    }
  }
}

static void ra_visit_r(struct ra *ra, struct ir *ir, struct ir_block *block) {
  /* use safe iterator to avoid iterating over fills inserted
     when rewriting arguments */
  list_for_each_entry_safe(instr, &block->instrs, struct ir_instr, it) {
    ra_expire_tmps(ra, ir, instr);

    for (int i = 0; i < MAX_INSTR_ARGS; i++) {
      ra_rewrite_arg(ra, ir, instr, i);
    }

    ra_alloc(ra, ir, instr->result);
  }

  list_for_each_entry(edge, &block->outgoing, struct ir_edge, it) {
    ra_push_state(ra);

    ra_visit_r(ra, ir, edge->dst);

    ra_pop_state(ra);
  }
}

static void ra_visit(struct ra *ra, struct ir *ir) {
  struct ir_block *head_block =
      list_first_entry(&ir->blocks, struct ir_block, it);
  ra_visit_r(ra, ir, head_block);
}

static void ra_create_temporaries_r(struct ra *ra, struct ir *ir,
                                    struct ir_block *block) {
  list_for_each_entry(instr, &block->instrs, struct ir_instr, it) {
    int ordinal = ra_get_ordinal(instr);

    if (instr->result) {
      struct ra_tmp *tmp = ra_create_tmp(ra, instr->result);
      ra_add_use(ra, tmp, ordinal);
    }

    for (int i = 0; i < MAX_INSTR_ARGS; i++) {
      struct ir_value *arg = instr->arg[i];

      if (!arg || ir_is_constant(arg)) {
        continue;
      }

      struct ra_tmp *tmp = ra_get_tmp(arg);
      ra_add_use(ra, tmp, ordinal);
    }
  }

  list_for_each_entry(edge, &block->outgoing, struct ir_edge, it) {
    ra_create_temporaries_r(ra, ir, edge->dst);
  }
}

static void ra_create_temporaries(struct ra *ra, struct ir *ir) {
  struct ir_block *head_block =
      list_first_entry(&ir->blocks, struct ir_block, it);
  ra_create_temporaries_r(ra, ir, head_block);
}

static void ra_assign_ordinals_r(struct ra *ra, struct ir *ir,
                                 struct ir_block *block, int *ordinal) {
  /* assign each instruction an ordinal. these ordinals are used to describe
     the live range of a particular value */
  list_for_each_entry(instr, &block->instrs, struct ir_instr, it) {
    ra_set_ordinal(instr, *ordinal);

    /* each instruction could fill up to MAX_INSTR_ARGS, space out ordinals
       enough to allow for this */
    (*ordinal) += 1 + MAX_INSTR_ARGS;
  }

  list_for_each_entry(edge, &block->outgoing, struct ir_edge, it) {
    ra_assign_ordinals_r(ra, ir, edge->dst, ordinal);
  }
}

static void ra_assign_ordinals(struct ra *ra, struct ir *ir) {
  int ordinal = 0;
  struct ir_block *head_block =
      list_first_entry(&ir->blocks, struct ir_block, it);
  ra_assign_ordinals_r(ra, ir, head_block, &ordinal);
}

static void ra_reset(struct ra *ra, struct ir *ir) {
  ra->num_uses = 0;
}

void ra_run(struct ra *ra, struct ir *ir) {
  ra_reset(ra, ir);

  ra_push_state(ra);

  ra_assign_ordinals(ra, ir);
  ra_create_temporaries(ra, ir);
  ra_visit(ra, ir);
#if 0
  ra_validate(ra, ir);
#endif

  ra_pop_state(ra);
}

void ra_destroy(struct ra *ra) {
  CHECK(list_empty(&ra->live_state));

  list_for_each_entry_safe(state, &ra->free_state, struct ra_state, it) {
    ra_destroy_state(ra, state);
  }

  free(ra->uses);
  free(ra);
}

struct ra *ra_create(const struct jit_register *regs, int num_regs) {
  struct ra *ra = calloc(1, sizeof(struct ra));

  ra->regs = regs;
  ra->num_regs = num_regs;

  return ra;
}
