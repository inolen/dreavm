#include "jit/ir/ir.h"
#include "core/string.h"

enum ir_token {
  TOK_EOF,
  TOK_EOL,
  TOK_OPERATOR,
  TOK_INTEGER,
  TOK_STRING,
  TOK_IDENTIFIER,
  TOK_TYPE,
  TOK_OP,
};

struct ir_reference {
  struct ir_instr *instr;
  int arg;
  enum ir_type type;
  char name[MAX_LABEL_SIZE];
  struct list_node it;
};

struct ir_lexeme {
  char s[128];
  uint64_t i;
  enum ir_op op;
  enum ir_type ty;
};

struct ir_parser {
  FILE *input;
  enum ir_token tok;
  struct ir_lexeme val;
  struct list refs;
};

static const char *typenames[] = {"",    "i8",  "i16",  "i32", "i64",
                                  "f32", "f64", "v128", "str", "blk"};
static const int num_typenames = sizeof(typenames) / sizeof(typenames[0]);

static int ir_lex_get(struct ir_parser *p) {
  return fgetc(p->input);
}

static void ir_lex_unget(struct ir_parser *p, int c) {
  ungetc(c, p->input);
}

static void ir_lex_next(struct ir_parser *p) {
  /* skip past whitespace characters, except newlines */
  int next;
  do {
    next = ir_lex_get(p);
  } while (isspace(next) && next != '\n');

  /* test for end of file */
  if (next == EOF) {
    strncpy(p->val.s, "", sizeof(p->val.s));
    p->tok = TOK_EOF;
    return;
  }

  /* test for newline */
  if (next == '\n') {
    strncpy(p->val.s, "\n", sizeof(p->val.s));

    /* chomp adjacent newlines */
    while (next == '\n') {
      next = ir_lex_get(p);
    }
    ir_lex_unget(p, next);

    p->tok = TOK_EOL;
    return;
  }

  /* test for assignment operator */
  if (next == ':' || next == ',' || next == '=') {
    snprintf(p->val.s, sizeof(p->val.s), "%c", next);
    p->tok = TOK_OPERATOR;
    return;
  }

  /* test for hex literal */
  if (next == '0') {
    next = ir_lex_get(p);

    if (next == 'x') {
      next = ir_lex_get(p);

      /* parse literal */
      p->val.i = 0;
      while (isxdigit(next)) {
        p->val.i <<= 4;
        p->val.i |= xtoi(next);
        next = ir_lex_get(p);
      }
      ir_lex_unget(p, next);

      p->tok = TOK_INTEGER;
      return;
    } else {
      ir_lex_unget(p, next);
    }
  }

  /* test for string literal */
  if (next == '\'') {
    next = ir_lex_get(p);

    char *ptr = p->val.s;
    while (next != '\'') {
      *ptr++ = (char)next;
      next = ir_lex_get(p);
    }
    *ptr = 0;

    p->tok = TOK_STRING;
    return;
  }

  /* treat anything else as an identifier */
  char *ptr = p->val.s;
  while (isalpha(next) || isdigit(next) || next == '%' || next == '_') {
    *ptr++ = (char)next;
    next = ir_lex_get(p);
  }
  ir_lex_unget(p, next);
  *ptr = 0;

  p->tok = TOK_IDENTIFIER;

  /* test for type keyword */
  for (int i = 1; i < num_typenames; i++) {
    const char *typename = typenames[i];

    if (!strcasecmp(p->val.s, typename)) {
      p->val.ty = i;
      p->tok = TOK_TYPE;
      return;
    }
  }

  /* test for op keyword */
  for (int i = 0; i < NUM_OPS; i++) {
    const char *opname = ir_op_names[i];

    if (!strcasecmp(p->val.s, opname)) {
      p->val.op = i;
      p->tok = TOK_OP;
      return;
    }
  }
}

static void ir_destroy_parser(struct ir_parser *p) {
  list_for_each_entry_safe(ref, &p->refs, struct ir_reference, it) {
    free(ref);
  }
}

static int ir_resolve_references(struct ir_parser *p, struct ir *ir) {
  list_for_each_entry(ref, &p->refs, struct ir_reference, it) {
    struct ir_value *value = NULL;

    if (ref->type == VALUE_BLOCK) {
      struct ir_block *found = NULL;

      list_for_each_entry(block, &ir->blocks, struct ir_block, it) {
        if (block->label && !strcmp(block->label, ref->name)) {
          found = block;
          break;
        }
      }

      if (!found) {
        LOG_INFO("Failed to resolve reference for %%%s", ref->name);
        return 0;
      }

      value = ir_alloc_block(ir, found);
    } else {
      struct ir_instr *found = NULL;
      list_for_each_entry(block, &ir->blocks, struct ir_block, it) {
        if (found) {
          break;
        }

        list_for_each_entry(instr, &block->instrs, struct ir_instr, it) {
          if (instr->label && !strcmp(instr->label, ref->name)) {
            found = instr;
            break;
          }
        }
      }

      if (!found) {
        LOG_INFO("Failed to resolve reference for %%%s", ref->name);
        return 0;
      }

      value = found->result;
    }

    ir_set_arg(ir, ref->instr, ref->arg, value);
  }

  return 1;
}

static void ir_defer_reference(struct ir_parser *p, struct ir_instr *instr,
                               int arg, enum ir_type type, const char *name) {
  struct ir_reference *ref = calloc(1, sizeof(struct ir_reference));
  ref->instr = instr;
  ref->arg = arg;
  ref->type = type;
  strncpy(ref->name, name, sizeof(ref->name));
  list_add(&p->refs, &ref->it);
}

static int ir_parse_type(struct ir_parser *p, struct ir *ir,
                         enum ir_type *type) {
  if (p->tok != TOK_TYPE) {
    LOG_INFO("Unexpected token %d when parsing type", p->tok);
    return 0;
  }

  /* eat token */
  ir_lex_next(p);

  *type = p->val.ty;

  return 1;
}

static int ir_parse_op(struct ir_parser *p, struct ir *ir, enum ir_op *op) {
  if (p->tok != TOK_OP) {
    LOG_INFO("Unexpected token %d when parsing op", p->tok);
    return 0;
  }

  /* eat token */
  ir_lex_next(p);

  *op = p->val.op;

  return 1;
}

static int ir_parse_operator(struct ir_parser *p, struct ir *ir) {
  const char *op_str = p->val.s;

  if (strcmp(op_str, "=")) {
    LOG_INFO("Unexpected operator '%s'", op_str);
    return 0;
  }

  /* eat token */
  ir_lex_next(p);

  /* nothing to do, there's only one operator token */

  return 1;
}

static int ir_parse_label(struct ir_parser *p, struct ir *ir, char *label) {
  if (p->tok != TOK_IDENTIFIER) {
    LOG_INFO("Unexpected token %d when parsing label", p->tok);
    return 0;
  }

  const char *ident = p->val.s;
  if (ident[0] != '%') {
    LOG_INFO("Expected label '%s' to begin with %%", ident);
    return 0;
  }
  strcpy(label, &ident[1]);
  ir_lex_next(p);

  return 1;
}

static int ir_parse_arg(struct ir_parser *p, struct ir *ir,
                        struct ir_instr *instr, int arg) {
  /* parse value type */
  enum ir_type type;
  if (!ir_parse_type(p, ir, &type)) {
    return 0;
  }

  /* parse value */
  if (p->tok == TOK_IDENTIFIER) {
    const char *ident = p->val.s;

    if (ident[0] != '%') {
      LOG_INFO("Expected identifier to begin with %%");
      return 0;
    }

    /* label reference, defer resolution until after all blocks / values have
       been parsed */
    const char *name = &ident[1];
    ir_defer_reference(p, instr, arg, type, name);
  } else if (p->tok == TOK_INTEGER || p->tok == TOK_STRING) {
    struct ir_value *value = NULL;

    switch (type) {
      case VALUE_I8: {
        uint8_t v = (uint8_t)p->val.i;
        value = ir_alloc_i8(ir, v);
      } break;
      case VALUE_I16: {
        uint16_t v = (uint16_t)p->val.i;
        value = ir_alloc_i16(ir, v);
      } break;
      case VALUE_I32: {
        uint32_t v = (uint32_t)p->val.i;
        value = ir_alloc_i32(ir, v);
      } break;
      case VALUE_I64: {
        uint64_t v = (uint64_t)p->val.i;
        value = ir_alloc_i64(ir, v);
      } break;
      case VALUE_F32: {
        uint32_t v = (uint32_t)p->val.i;
        value = ir_alloc_f32(ir, *(float *)&v);
      } break;
      case VALUE_F64: {
        uint64_t v = (uint64_t)p->val.i;
        value = ir_alloc_f64(ir, *(double *)&v);
      } break;
      case VALUE_STRING: {
        value = ir_alloc_str(ir, p->val.s);
      } break;
      default:
        LOG_FATAL("Unexpected value type");
        break;
    }

    ir_set_arg(ir, instr, arg, value);
  } else {
    LOG_INFO("Unexpected token %d when parsing value: %s", p->tok, p->val.s);
    return 0;
  }

  /* eat token */
  ir_lex_next(p);

  return 1;
}

static int ir_parse_instr(struct ir_parser *p, struct ir *ir) {
  enum ir_type type = VALUE_V;
  char label[MAX_LABEL_SIZE] = {0};

  /* parse result type and label */
  if (p->tok == TOK_TYPE) {
    if (!ir_parse_type(p, ir, &type)) {
      return 0;
    }

    if (!ir_parse_label(p, ir, label)) {
      return 0;
    }

    if (!ir_parse_operator(p, ir)) {
      return 0;
    }
  }

  /* parse op */
  enum ir_op op;
  if (!ir_parse_op(p, ir, &op)) {
    return 0;
  }

  /* create instruction */
  struct ir_instr *instr = ir_append_instr(ir, op, type);

  /* parse arguments */
  if (p->tok == TOK_TYPE) {
    for (int i = 0; i < MAX_INSTR_ARGS; i++) {
      if (!ir_parse_arg(p, ir, instr, i)) {
        return 0;
      }

      if (p->tok != TOK_OPERATOR) {
        break;
      }

      /* eat comma and move onto the next argument */
      ir_lex_next(p);
    }
  }

  if (label[0]) {
    ir_set_instr_label(ir, instr, label);
  }

  return 1;
}

static int ir_parse_block(struct ir_parser *p, struct ir *ir) {
  if (p->tok != TOK_IDENTIFIER) {
    LOG_INFO("Unexpected token %d when parsing block", p->tok);
    return 0;
  }

  char label[MAX_LABEL_SIZE];
  if (!ir_parse_label(p, ir, label)) {
    return 0;
  }

  if (p->tok != TOK_OPERATOR || p->val.s[0] != ':') {
    LOG_INFO("Expected label to be followed by : operator");
    return 0;
  }
  ir_lex_next(p);

  struct ir_block *block = ir_append_block(ir);
  ir_set_block_label(ir, block, label);
  ir_set_current_block(ir, block);

  return 1;
}

int ir_read(FILE *input, struct ir *ir) {
  struct ir_parser p = {0};
  p.input = input;

  int res = 1;

  while (1) {
    ir_lex_next(&p);

    if (p.tok == TOK_EOF) {
      if (!ir_resolve_references(&p, ir)) {
        res = 0;
      }
      break;
    }

    if (p.tok == TOK_IDENTIFIER) {
      if (!ir_parse_block(&p, ir)) {
        res = 0;
        break;
      }
    } else {
      if (!ir_parse_instr(&p, ir)) {
        res = 0;
        break;
      }
    }
  }

  ir_destroy_parser(&p);

  return res;
}
