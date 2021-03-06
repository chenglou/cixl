#include "cixl/box.h"
#include "cixl/cx.h"
#include "cixl/error.h"
#include "cixl/scope.h"
#include "cixl/tok.h"
#include "cixl/types/vect.h"
#include "cixl/var.h"

struct cx_scope *cx_scope_new(struct cx *cx, struct cx_scope *parent) {
  struct cx_scope *scope = cx_malloc(&cx->scope_alloc);
  scope->cx = cx;
  scope->parent = parent ? cx_scope_ref(parent) : NULL;
  cx_vec_init(&scope->stack, sizeof(struct cx_box));

  cx_set_init(&scope->env, sizeof(struct cx_var), cx_cmp_sym);
  scope->env.key_offs = offsetof(struct cx_var, id);

  cx_vec_init(&scope->cuts, sizeof(struct cx_cut));
  scope->safe = cx->scopes.count ? cx_scope(cx, 0)->safe : true;
  scope->nrefs = 0;
  return scope;
}

struct cx_scope *cx_scope_ref(struct cx_scope *scope) {
  scope->nrefs++;
  return scope;
}

void cx_scope_deref(struct cx_scope *scope) {
  cx_test(scope->nrefs);
  scope->nrefs--;
  
  if (!scope->nrefs) {
    if (scope->parent) { cx_scope_deref(scope->parent); }

    cx_vec_deinit(&scope->cuts);

    cx_do_vec(&scope->stack, struct cx_box, b) { cx_box_deinit(b); }
    cx_vec_deinit(&scope->stack);
    
    cx_do_set(&scope->env, struct cx_var, v) { cx_var_deinit(v); }
    cx_set_deinit(&scope->env);

    cx_free(&scope->cx->scope_alloc, scope);
  }
}

struct cx_box *cx_push(struct cx_scope *scope) {
  return cx_vec_push(&scope->stack);
}

struct cx_box *cx_pop(struct cx_scope *scope, bool silent) {
  if (!scope->stack.count) {
    if (!silent) {
      cx_error(scope->cx, scope->cx->row, scope->cx->col, "Stack is empty");
    }

    return NULL;
  }

  return cx_vec_pop(&scope->stack);
}

struct cx_box *cx_peek(struct cx_scope *scope, bool silent) {
  if (!scope->stack.count) {
    if (silent) { return NULL; }
    cx_error(scope->cx, scope->cx->row, scope->cx->col, "Stack is empty");
  }

  return cx_vec_peek(&scope->stack, 0);
}

void cx_stackdump(struct cx_scope *scope, FILE *out) {
  cx_vect_dump(&scope->stack, out);
  fputc('\n', out);
}

struct cx_box *cx_get_var(struct cx_scope *scope, struct cx_sym id, bool silent) {
  struct cx_var *var = cx_set_get(&scope->env, &id);

  if (!var) {
    if (scope->parent) { return cx_get_var(scope->parent, id, silent); }

    if (!silent) {
      struct cx *cx = scope->cx;
      cx_error(cx, cx->row, cx->col, "Unknown var: %s", id.id);
    }
    
    return NULL;
  }

  return &var->value;
}

struct cx_box *cx_put_var(struct cx_scope *scope, struct cx_sym id, bool force) {
  struct cx_var *var = NULL;

  void *found = NULL;
  size_t i = cx_set_find(&scope->env, &id, 0, &found);
  
  if (found) {
    var = found;

    if (!force) {
      struct cx *cx = scope->cx;
      cx_error(cx, cx->row, cx->col, "Attempt to rebind var: %s", id.id);
      return NULL;
    }
      
    cx_box_deinit(&var->value);
  } else {
    var = cx_var_init(cx_vec_insert(&scope->env.members, i), id);
  }

  return &var->value;
}

bool cx_delete_var(struct cx_scope *scope, struct cx_sym id, bool silent) {
  struct cx_var *v = cx_set_get(&scope->env, &id);

  if (!v) {
    if (!silent) {
      struct cx *cx = scope->cx;
      cx_error(cx, cx->row, cx->col, "Unknown var: %s", id.id);
    }

    return false;
  }

  cx_var_deinit(v);
  cx_set_delete(&scope->env, &id);
  return true;
}

struct cx_cut *cx_cut_init(struct cx_cut *cut, struct cx_scope *scope) {
  cut->scope = scope;
  cut->offs = scope->stack.count;
  scope->cx->scan_level++;
  cut->scan_level = scope->cx->scan_level;
  return cut;
}

struct cx_cut *cx_cut_deinit(struct cx_cut *cut) {
  cut->scope->cx->scan_level--;
  return cut;
}
