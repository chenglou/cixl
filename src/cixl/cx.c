#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "cixl/bin.h"
#include "cixl/box.h"
#include "cixl/call.h"
#include "cixl/cx.h"
#include "cixl/error.h"
#include "cixl/eval.h"
#include "cixl/op.h"
#include "cixl/scope.h"
#include "cixl/timer.h"
#include "cixl/types/bin.h"
#include "cixl/types/bool.h"
#include "cixl/types/char.h"
#include "cixl/types/cmp.h"
#include "cixl/types/file.h"
#include "cixl/types/fimp.h"
#include "cixl/types/func.h"
#include "cixl/types/guid.h"
#include "cixl/types/int.h"
#include "cixl/types/iter.h"
#include "cixl/types/lambda.h"
#include "cixl/types/nil.h"
#include "cixl/types/pair.h"
#include "cixl/types/rat.h"
#include "cixl/types/rec.h"
#include "cixl/types/str.h"
#include "cixl/types/sym.h"
#include "cixl/types/table.h"
#include "cixl/types/time.h"
#include "cixl/types/vect.h"
#include "cixl/util.h"
#include "cixl/var.h"

static const void *get_type_id(const void *value) {
  struct cx_type *const *type = value;
  return &(*type)->id;
}

static const void *get_macro_id(const void *value) {
  struct cx_macro *const *macro = value;
  return &(*macro)->id;
}

static const void *get_func_id(const void *value) {
  struct cx_func *const *func = value;
  return &(*func)->id;
}

static bool read_imp(struct cx_scope *scope) {
  struct cx *cx = scope->cx;
  struct cx_box in = *cx_test(cx_pop(scope, false));

  struct cx_vec toks;
  cx_vec_init(&toks, sizeof(struct cx_tok));
  bool ok = false;
  
  if (!cx_parse_tok(cx, in.as_file->ptr, &toks, true)) {
    if (!cx->errors.count) { cx_box_init(cx_push(scope), cx->nil_type); }
    goto exit1;
  }
  
  struct cx_bin bin;
  cx_bin_init(&bin);
  
  if (!cx_compile(cx, cx_vec_start(&toks), cx_vec_end(&toks), &bin)) { goto exit2; }
  if (!cx_eval(cx, &bin, NULL)) { goto exit2; }
  ok = true;
 exit2:
  cx_bin_deinit(&bin);
 exit1:
  cx_box_deinit(&in);
  cx_do_vec(&toks, struct cx_tok, t) { cx_tok_deinit(t); }
  cx_vec_deinit(&toks);
  return ok;
}

static bool write_imp(struct cx_scope *scope) {
  struct cx_box
    out = *cx_test(cx_pop(scope, false)),
    v = *cx_test(cx_pop(scope, false));
  
  bool ok = cx_write(&v, out.as_file->ptr);
  fputc('\n', out.as_file->ptr);
  cx_box_deinit(&v);
  cx_box_deinit(&out);
  return ok;
}
  
static bool compile_imp(struct cx_scope *scope) {
  struct cx *cx = scope->cx;
  
  struct cx_box
    in = *cx_test(cx_pop(scope, false)),
    out = *cx_test(cx_pop(scope, false));

  struct cx_vec toks;
  cx_vec_init(&toks, sizeof(struct cx_tok));
  bool ok = cx_parse_str(cx, in.as_str->data, &toks);
  if (!ok) { goto exit; }
  
  struct cx_bin *bin = out.as_ptr;

  if (!(ok = cx_compile(cx, cx_vec_start(&toks), cx_vec_end(&toks), bin))) {
    goto exit;
  }
 exit:
  cx_box_deinit(&in);
  cx_box_deinit(&out);
  cx_do_vec(&toks, struct cx_tok, t) { cx_tok_deinit(t); }
  cx_vec_deinit(&toks);
  return ok;
}

static bool call_imp(struct cx_scope *scope) {
  struct cx_box v = *cx_test(cx_pop(scope, false));
  bool ok = cx_call(&v, scope);
  cx_box_deinit(&v);
  return ok;
}

static bool new_imp(struct cx_scope *scope) {
  struct cx *cx = scope->cx;
  struct cx_type *t = cx_test(cx_pop(scope, false))->as_ptr;

  if (!t->new) {
    cx_error(cx, cx->row, cx->col, "%s does not implement new", t->id);
    return false;
  }
  
  struct cx_box *v = cx_push(scope);
  v->type = t;
  t->new(v);
  return true;
}

static bool clock_imp(struct cx_scope *scope) {
  struct cx_box v = *cx_test(cx_pop(scope, false));
  cx_timer_t timer;
  cx_timer_reset(&timer);
  bool ok = cx_call(&v, scope);
  cx_box_deinit(&v);
  cx_box_init(cx_push(scope), scope->cx->int_type)->as_int = cx_timer_ns(&timer);
  return ok;
}

static bool test_imp(struct cx_scope *scope) {
  struct cx_box v = *cx_test(cx_pop(scope, false));
  struct cx *cx = scope->cx;
  bool ok = true;
  
  if (!cx_ok(&v)) {
    cx_error(cx, cx->row, cx->col, "Test failed");
    ok = false;
  }

  cx_box_deinit(&v);
  return ok;
}

static bool fail_imp(struct cx_scope *scope) {
  struct cx_box m = *cx_test(cx_pop(scope, false));
  struct cx *cx = scope->cx;
  cx_error(cx, cx->row, cx->col, m.as_str->data);
  cx_box_deinit(&m);
  return false;
}

struct cx *cx_init(struct cx *cx) {
  cx->inline_limit1 = 10;
  cx->inline_limit2 = -1;
  cx->next_sym_tag = cx->next_type_tag = 1;
  cx->bin = NULL;
  cx->op = NULL;
  cx->scan_depth = 0;
  cx->stop = false;
  cx->row = cx->col = -1;
  
  cx_set_init(&cx->separators, sizeof(char), cx_cmp_char);
  cx_add_separators(cx, " \t\n;,.|_?!()[]{}");

  cx_set_init(&cx->syms, sizeof(struct cx_sym), cx_cmp_str);
  cx->syms.key_offs = offsetof(struct cx_sym, id);

  cx_set_init(&cx->types, sizeof(struct cx_type *), cx_cmp_str);
  cx->types.key = get_type_id;
  
  cx_set_init(&cx->macros, sizeof(struct cx_macro *), cx_cmp_str);
  cx->macros.key = get_macro_id;

  cx_set_init(&cx->funcs, sizeof(struct cx_func *), cx_cmp_str);
  cx->funcs.key = get_func_id;

  cx_set_init(&cx->consts, sizeof(struct cx_var), cx_cmp_sym);
  cx->consts.key_offs = offsetof(struct cx_var, id);

  cx_malloc_init(&cx->lambda_alloc, CX_LAMBDA_SLAB_SIZE, sizeof(struct cx_lambda));
  cx_malloc_init(&cx->pair_alloc, CX_PAIR_SLAB_SIZE, sizeof(struct cx_pair));
  cx_malloc_init(&cx->rec_alloc, CX_REC_SLAB_SIZE, sizeof(struct cx_rec));
  cx_malloc_init(&cx->scope_alloc, CX_SCOPE_SLAB_SIZE, sizeof(struct cx_scope));
  cx_malloc_init(&cx->table_alloc, CX_TABLE_SLAB_SIZE, sizeof(struct cx_table));
  
  cx_vec_init(&cx->scopes, sizeof(struct cx_scope *));
  cx_vec_init(&cx->calls, sizeof(struct cx_call));
  cx_vec_init(&cx->errors, sizeof(struct cx_error));
  
  cx->opt_type = cx_add_type(cx, "Opt");
  cx->opt_type->trait = true;

  cx->any_type = cx_add_type(cx, "A", cx->opt_type);
  cx->any_type->trait = true;

  cx->cmp_type = cx_init_cmp_type(cx);

  cx->seq_type = cx_add_type(cx, "Seq");
  cx->seq_type->trait = true;

  cx->num_type = cx_add_type(cx, "Num", cx->cmp_type);
  cx->num_type->trait = true;
  
  cx->rec_type = cx_add_type(cx, "Rec", cx->any_type, cx->cmp_type);
  cx->rec_type->trait = true;

  cx->nil_type = cx_init_nil_type(cx);
  cx->meta_type = cx_init_meta_type(cx);
  
  cx->pair_type = cx_init_pair_type(cx);
  cx->iter_type = cx_init_iter_type(cx);
  cx->bool_type = cx_init_bool_type(cx);
  cx->int_type = cx_init_int_type(cx);
  cx->rat_type = cx_init_rat_type(cx);
  cx->char_type = cx_init_char_type(cx);
  cx->str_type = cx_init_str_type(cx);
  cx->sym_type = cx_init_sym_type(cx);
  cx->time_type = cx_init_time_type(cx);
  cx->guid_type = cx_init_guid_type(cx);
  cx->vect_type = cx_init_vect_type(cx);
  cx->table_type = cx_init_table_type(cx);
  cx->bin_type = cx_init_bin_type(cx);
  cx->func_type = cx_init_func_type(cx);
  cx->fimp_type = cx_init_fimp_type(cx);
  cx->lambda_type = cx_init_lambda_type(cx);

  cx->file_type = cx_init_file_type(cx, "File");
  cx->rfile_type = cx_init_file_type(cx, "RFile", cx->any_type, cx->file_type);
  cx->wfile_type = cx_init_file_type(cx, "WFile", cx->any_type, cx->file_type);
  cx->rwfile_type = cx_init_file_type(cx, "RWFile", cx->rfile_type, cx->wfile_type);
  
  cx_add_cfunc(cx, "read", read_imp, cx_arg("f", cx->rfile_type));

  cx_add_cfunc(cx, "write", write_imp,
	       cx_arg("v", cx->opt_type), cx_arg("f", cx->wfile_type));

  cx_add_cfunc(cx, "compile", compile_imp,
	       cx_arg("out", cx->bin_type), cx_arg("in", cx->str_type));

  cx_add_cfunc(cx, "call", call_imp, cx_arg("act", cx->any_type));
  cx_add_cfunc(cx, "new", new_imp, cx_arg("t", cx->meta_type));
  cx_add_cfunc(cx, "clock", clock_imp, cx_arg("act", cx->any_type));
  cx_add_cfunc(cx, "test", test_imp, cx_arg("v", cx->opt_type));
  cx_add_cfunc(cx, "fail", fail_imp, cx_arg("msg", cx->str_type));

  cx->scope = NULL;
  cx->main = cx_begin(cx, NULL);
  srand((ptrdiff_t)cx + clock());

  return cx;
}

struct cx *cx_deinit(struct cx *cx) {
  cx_set_deinit(&cx->separators);
  
  cx_do_vec(&cx->errors, char *, e) { free(*e); }
  cx_vec_deinit(&cx->errors);

  cx_do_vec(&cx->calls, struct cx_call, c) { cx_call_deinit(c); }
  cx_vec_deinit(&cx->calls);

  cx_do_vec(&cx->scopes, struct cx_scope *, s) { cx_scope_deref(*s); }
  cx_vec_deinit(&cx->scopes);

  cx_do_set(&cx->consts, struct cx_var, v) { cx_var_deinit(v); }
  cx_set_deinit(&cx->consts);

  cx_do_set(&cx->macros, struct cx_macro *, m) { free(cx_macro_deinit(*m)); }
  cx_set_deinit(&cx->macros);

  cx_do_set(&cx->funcs, struct cx_func *, f) { free(cx_func_deinit(*f)); }
  cx_set_deinit(&cx->funcs);

  cx_do_set(&cx->types, struct cx_type *, t) { free(cx_type_deinit(*t)); }
  cx_set_deinit(&cx->types);

  cx_do_set(&cx->syms, struct cx_sym, s) { cx_sym_deinit(s); }
  cx_set_deinit(&cx->syms);

  cx_malloc_deinit(&cx->lambda_alloc);
  cx_malloc_deinit(&cx->pair_alloc);
  cx_malloc_deinit(&cx->rec_alloc);
  cx_malloc_deinit(&cx->scope_alloc);
  cx_malloc_deinit(&cx->table_alloc);
  return cx;
}

void cx_add_separators(struct cx *cx, const char *cs) {
  for (const char *c = cs; *c; c++) {
    *(char *)cx_test(cx_set_insert(&cx->separators, c)) = *c;
  }
}

bool cx_is_separator(struct cx *cx, char c) {
  return cx_set_get(&cx->separators, &c);
}

struct cx_type *_cx_add_type(struct cx *cx, const char *id, ...) {
  va_list parents;
  va_start(parents, id);				
  struct cx_type *t = cx_vadd_type(cx, id, parents);
  va_end(parents);					
  return t;
}
 
struct cx_type *cx_vadd_type(struct cx *cx, const char *id, va_list parents) {
  struct cx_type **t = cx_test(cx_set_insert(&cx->types, &id));

  if (!t) {
    cx_error(cx, cx->row, cx->col, "Duplicate type: '%s'", id);
    return NULL;
  }
  
  *t = cx_type_init(malloc(sizeof(struct cx_type)), cx, id);
    
  struct cx_type *pt = NULL;
  while ((pt = va_arg(parents, struct cx_type *))) { cx_derive(*t, pt); }
  return *t;
}

struct cx_rec_type *cx_add_rec_type(struct cx *cx, const char *id) {
  struct cx_type **found = cx_set_get(&cx->types, &id);
  if (found) { return NULL; }
  
  if (found) {
    struct cx_rec_type *t = cx_baseof(*found, struct cx_rec_type, imp);
    cx_rec_type_reinit(t);
    return t;
  }
  
  struct cx_rec_type *t = cx_rec_type_new(cx, id);
  *(struct cx_type **)cx_test(cx_set_insert(&cx->types, &id)) = &t->imp;
  return t;
}

struct cx_type *cx_get_type(struct cx *cx, const char *id, bool silent) {
  struct cx_type **t = cx_set_get(&cx->types, &id);

  if (!t && !silent) {
    cx_error(cx, cx->row, cx->col, "Unknown type: '%s'", id);
  }

  return t ? *t : NULL;
}

struct cx_fimp *_cx_add_func(struct cx *cx,
			     const char *id,
			     int nargs,
			     struct cx_func_arg *args) {
  struct cx_func **f = cx_set_get(&cx->funcs, &id);

  if (f) {
    if ((*f)->nargs != nargs) {
      cx_error(cx,
	       cx->row, cx->col,
	       "Wrong number of args for func '%s': %d/%d",
	       id, nargs, (*f)->nargs);
    }
  } else {
    f = cx_set_insert(&cx->funcs, &id);
    *f = cx_func_init(malloc(sizeof(struct cx_func)), cx, id, nargs);
  }
  
  return cx_func_add_imp(*f, nargs, args);
}

bool cx_add_mixl_func(struct cx *cx,
		      const char *id,
		      const char *args,
		      const char *body) {
  char *in = cx_fmt("func: %s(%s) %s;", id, args, body);
  bool ok = cx_eval_str(cx, in);
  free(in);
  return ok;
}

struct cx_func *cx_get_func(struct cx *cx, const char *id, bool silent) {
  struct cx_func **f = cx_set_get(&cx->funcs, &id);

  if (!f && !silent) {
    cx_error(cx, cx->row, cx->col, "Unknown func: '%s'", id);
  }

  return f ? *f : NULL;
}


struct cx_macro *cx_add_macro(struct cx *cx, const char *id, cx_macro_parse_t imp) {
  struct cx_macro **m = cx_test(cx_set_insert(&cx->macros, &id));

  if (!m) {
    cx_error(cx, cx->row, cx->col, "Duplicate macro: '%s'", id);
    return NULL;
  }

  *m = cx_macro_init(malloc(sizeof(struct cx_macro)), id, imp); 
  return *m;
}

struct cx_macro *cx_get_macro(struct cx *cx, const char *id, bool silent) {
  struct cx_macro **m = cx_set_get(&cx->macros, &id);

  if (!m && !silent) {
    cx_error(cx, cx->row, cx->col, "Unknown macro: '%s'", id);
  }
  
  return m ? *m : NULL;
}

struct cx_box *cx_get_const(struct cx *cx, struct cx_sym id, bool silent) {
  struct cx_var *var = cx_set_get(&cx->consts, &id);

  if (!var) {
    if (!silent) { cx_error(cx, cx->row, cx->col, "Unknown const: '%s'", id); }
    return NULL;
  }

  return &var->value;
}

struct cx_box *cx_set_const(struct cx *cx, struct cx_sym id, bool force) {
  struct cx_var *var = cx_set_get(&cx->consts, &id);

  if (var) {
    if (!force) {
      cx_error(cx, cx->row, cx->col, "Attempt to rebind const: '%s'", id);
      return NULL;
    }
      
    cx_box_deinit(&var->value);
  } else {
    var = cx_var_init(cx_set_insert(&cx->consts, &id), id);
  }

  return &var->value;
}

struct cx_sym cx_sym(struct cx *cx, const char *id) {
  struct cx_sym *s = cx_set_get(&cx->syms, &id);
  return s ? *s : *cx_sym_init(cx_set_insert(&cx->syms, &id), id, cx->next_sym_tag++);
}

struct cx_scope *cx_scope(struct cx *cx, size_t i) {
  cx_test(cx->scopes.count > i);
  return *(cx->scope-i);
}

void cx_push_scope(struct cx *cx, struct cx_scope *scope) {
  cx->scope = cx_vec_push(&cx->scopes);
  *cx->scope = cx_scope_ref(scope);
}

struct cx_scope *cx_pop_scope(struct cx *cx, bool silent) {
  if (cx->scopes.count == 1) {
    if (!silent) { cx_error(cx, cx->row, cx->col, "No open scopes"); }
    return NULL;
  }
  
  struct cx_scope *s = *cx->scope;
  cx->scope--;
  cx_vec_pop(&cx->scopes);

  if (s->stack.count) {
    struct cx_box *v = cx_vec_pop(&s->stack);
    *cx_push(cx_scope(cx, 0)) = *v;   
  }

  cx_scope_deref(s);
  return s;
}

struct cx_scope *cx_begin(struct cx *cx, struct cx_scope *parent) {
  struct cx_scope *s = cx_scope_new(cx, parent);
  cx_push_scope(cx, s);
  return s;
}

void cx_end(struct cx *cx) {
  cx_pop_scope(cx, false);
}

bool cx_funcall(struct cx *cx, const char *id) {
  struct cx_func *func = cx_get_func(cx, id, false);
  if (!func) { return false; }
  struct cx_scope *s = cx_scope(cx, 0);
  struct cx_fimp *imp = cx_func_get_imp(func, &s->stack, 0);
  
  if (!imp) {
    cx_error(cx, cx->row, cx->col, "Func not applicable: %s", func->id);
    return false;
  }
  
  return cx_fimp_call(imp, s);
}

bool cx_load(struct cx *cx, const char *path) {
  FILE *f = fopen(path , "r");

  if (!f) {
    cx_error(cx, cx->row, cx->col, "Failed opening file '%s': %d", path, errno);
    return false;
  }
  
  fseek(f, 0, SEEK_END);
  size_t len = ftell(f);
  rewind(f);

  char *buf = malloc(len+1);
  
  if (fread(buf, len, 1 , f) != 1) {
    cx_error(cx, cx->row, cx->col, "Failed reading file '%s': %d", path, errno);
  }
  
  fclose(f);
  buf[len] = 0;
  bool ok = cx_eval_str(cx, buf);
  free(buf);
  return ok;
}
