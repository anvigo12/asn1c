/*
 * Resolve every reference of the tree for tree printers
 * (asn1c -E -F -print-json; fixer flag A1F_RESOLVE_ALL_REFS).
 *
 * The fixer resolves a reference only when the code generator needs it.
 * This pass sets the target of each reference that has none:
 * asn1p_ref_t.ref_expr, or ref_param_owner and ref_param_index for a
 * DummyReference inside a parameterized assignment. It does not change
 * the tree otherwise. It does not create specializations: a lookup never
 * passes actual parameters.
 *
 * Reference classes:
 *   type, value, class, object and object set references (lookup in the
 *   module of the reference, with the IMPORTS rules of the fixer);
 *   DummyReferences (formal parameters of the enclosing parameterized
 *   assignment);
 *   identifiers in WITH COMPONENT(S) (components of the constrained type,
 *   X.680 51.8.6);
 *   identifiers of named numbers, enumerations and named bits in values
 *   (members of the governing type);
 *   component relation references "@a.b", "@.a" (components of the
 *   enclosing types).
 * A reference that the pass cannot resolve is a FATAL diagnostic.
 */
#include "asn1fix_internal.h"
#include "asn1fix_xref.h"

#define XREF_MAX_DEPTH 512

typedef struct xref_s {
    arg_t *arg;            /* Diagnostics (arg->eh) */
    arg_t look;            /* Quiet copy of arg for lookups */
    asn1p_expr_t *generic; /* Parameterized assignment in scope, or NULL */
    asn1p_expr_t *outer[XREF_MAX_DEPTH]; /* Enclosing constructions, outermost first:
                                          * SET, SET OF, SEQUENCE, SEQUENCE OF, CHOICE */
    int nouter;
    int obase;             /* Index of the outermost type in outer[] */
    const asn1p_expr_t *top; /* Assignment being processed */
    const asn1p_expr_t *visiting[XREF_MAX_DEPTH];
    int depth;
    int failed;
} xref_t;

static void xr_expr(xref_t *x, asn1p_expr_t *e);
static void xr_constraint(xref_t *x, asn1p_constraint_t *ct, asn1p_expr_t *type);
static void xr_value(xref_t *x, asn1p_value_t *v, asn1p_expr_t *type);

static int
xr_resolved(const asn1p_ref_t *ref) {
    return ref->ref_expr != NULL || ref->ref_param_owner != NULL;
}

static void
xr_unresolved(xref_t *x, const asn1p_ref_t *ref, const char *what) {
    arg_t *arg = x->arg;
    asn1p_module_t *saved = arg->mod;
    if(ref->module) arg->mod = ref->module;
    FATAL("Cannot resolve %s \"%s\" at line %d (in %s%s%s) (-print-json)",
          what, asn1f_printable_reference(ref), ref->_lineno,
          x->top && x->top->module ? x->top->module->ModuleName : "?",
          x->top ? "." : "", x->top && x->top->Identifier ? x->top->Identifier : "");
    arg->mod = saved;
    x->failed = 1;
}

/*
 * Lookup in the module of the reference, without diagnostics. A
 * parameterized type named with actual parameters resolves to its
 * existing specialization for equal actual parameters, else to its
 * generic form. The lookup never creates a specialization.
 */
static asn1p_expr_t *
xr_lookup(xref_t *x, asn1p_ref_t *ref, asn1p_expr_t *rhs_pspecs) {
    asn1p_expr_t *target;
    asn1p_expr_t scratch; /* The lookup marks and reports arg->expr */
    int saved_errno = errno;
    int saved_generic = asn1f_generic_lookups(1);
    memset(&scratch, 0, sizeof(scratch));
    scratch._lineno = ref->_lineno;
    scratch.module = ref->module ? ref->module : x->arg->mod;
    x->look.mod = x->arg->mod;
    x->look.ns = x->arg->ns;
    x->look.expr = &scratch;
    target = asn1f_lookup_symbol(&x->look, NULL, ref);
    asn1f_generic_lookups(saved_generic);
    errno = saved_errno;
    if(target && rhs_pspecs && target->lhs_params && target->spec_index == -1) {
        for(int i = 0; i < target->specializations.pspecs_count; i++) {
            struct asn1p_pspec_s *ps = &target->specializations.pspec[i];
            if(ps->my_clone && ps->rhs_pspecs
               && asn1p_expr_compare(rhs_pspecs, ps->rhs_pspecs) == 0) {
                target = ps->my_clone;
                break;
            }
        }
        ref->ref_expr = target;
    }
    return target;
}

/* A DummyReference of the parameterized assignment in scope. */
static int
xr_dummy(xref_t *x, asn1p_ref_t *ref) {
    if(!x->generic || !x->generic->lhs_params || ref->comp_count < 1)
        return 0;
    for(int i = 0; i < x->generic->lhs_params->params_count; i++) {
        const char *arg_name = x->generic->lhs_params->params[i].argument;
        if(arg_name && strcmp(arg_name, ref->components[0].name) == 0) {
            ref->ref_param_owner = x->generic;
            ref->ref_param_index = i;
            return 1;
        }
    }
    return 0;
}

/*
 * A type, value, class, object, or object set reference. rhs_pspecs:
 * the actual parameters that follow the reference, or NULL.
 */
static void
xr_ref_args(xref_t *x, asn1p_ref_t *ref, asn1p_expr_t *rhs_pspecs,
            const char *what) {
    if(!ref || xr_resolved(ref)) return;
    if(xr_dummy(x, ref)) return;
    if(xr_lookup(x, ref, rhs_pspecs)) return; /* The lookup sets ref->ref_expr */
    xr_unresolved(x, ref, what);
}

static void
xr_ref(xref_t *x, asn1p_ref_t *ref, const char *what) {
    xr_ref_args(x, ref, NULL, what);
}

/*
 * The terminal type of a type: follow type references without creating
 * specializations. NULL when the chain ends at a DummyReference or cannot
 * be followed.
 */
static asn1p_expr_t *
xr_terminal(xref_t *x, asn1p_expr_t *type) {
    for(int n = 0; type && n < 32; n++) {
        asn1p_ref_t *ref = type->reference;
        if(type->expr_type != A1TC_REFERENCE || !ref) return type;
        if(!xr_resolved(ref)) xr_ref(x, ref, "type reference");
        if(ref->ref_param_owner || !ref->ref_expr) return NULL;
        type = ref->ref_expr;
    }
    return NULL;
}

/* Member with the given identifier, also inside extension addition groups. */
static asn1p_expr_t *
xr_member(asn1p_expr_t *type, const char *name) {
    asn1p_expr_t *m;
    if(!type || !name) return NULL;
    TQ_FOR(m, &(type->members), next) {
        if(m->Identifier && strcmp(m->Identifier, name) == 0) return m;
        if(!m->Identifier && m->ext_group > 0) {
            asn1p_expr_t *gm = xr_member(m, name);
            if(gm) return gm;
        }
    }
    return NULL;
}

/* True if the type is, or is a reference chain to, a DummyReference. */
static int
xr_is_dummy_type(xref_t *x, asn1p_expr_t *type) {
    for(int n = 0; type && n < 32; n++) {
        asn1p_ref_t *ref = type->reference;
        if(type->expr_type != A1TC_REFERENCE || !ref) return 0;
        if(ref->ref_param_owner) return 1;
        if(!ref->ref_expr) return 0;
        type = ref->ref_expr;
    }
    (void)x;
    return 0;
}

/*
 * An identifier that names a component of a type. For a type that a
 * DummyReference gives, the component is known only for each
 * specialization: the identifier then refers to the formal parameter.
 */
static asn1p_expr_t *
xr_component_ref(xref_t *x, asn1p_ref_t *ref, asn1p_expr_t *type,
                 const char *name, const char *what) {
    asn1p_expr_t *comp;
    if(xr_resolved(ref)) return ref->ref_expr;
    comp = xr_member(xr_terminal(x, type), name);
    if(comp) {
        ref->ref_expr = comp;
        return comp;
    }
    if(type && xr_is_dummy_type(x, type)) {
        asn1p_ref_t *dref = NULL;
        asn1p_expr_t *t = type;
        for(int n = 0; t && t->reference && n < 32; n++, t = t->reference->ref_expr) {
            dref = t->reference;
            if(dref->ref_param_owner) break;
        }
        if(dref && dref->ref_param_owner) {
            ref->ref_param_owner = dref->ref_param_owner;
            ref->ref_param_index = dref->ref_param_index;
            return NULL;
        }
    }
    xr_unresolved(x, ref, what);
    return NULL;
}

static int
xr_is_set_or_sequence(const asn1p_expr_t *e) {
    return e->expr_type == ASN_CONSTR_SEQUENCE || e->expr_type == ASN_CONSTR_SET;
}

/*
 * The parent structure of an AtNotation (X.682 (02/2021) 10.10):
 * a) "@a.b": the outermost textually enclosing set, sequence or choice type;
 * b) "@.a", "@..a", ...: from the innermost textually enclosing set or
 *    sequence type, up by one construction (set, set-of, sequence,
 *    sequence-of, choice) for each additional ".".
 */
static asn1p_expr_t *
xr_at_parent(xref_t *x, int dots) {
    int i;
    if(dots == 0) {
        for(i = x->obase; i < x->nouter; i++)
            if(x->outer[i]->expr_type != ASN_CONSTR_SEQUENCE_OF
               && x->outer[i]->expr_type != ASN_CONSTR_SET_OF)
                return x->outer[i];
        return NULL;
    }
    for(i = x->nouter - 1; i >= x->obase; i--)
        if(xr_is_set_or_sequence(x->outer[i])) break;
    i -= dots - 1;
    return (i >= x->obase && i < x->nouter) ? x->outer[i] : NULL;
}

/* Component relation "@a.b" or "@.a", "@..a", ... (X.682 (02/2021) 10.7 to 10.10). */
static void
xr_at_ref(xref_t *x, asn1p_ref_t *ref) {
    const char *name;
    asn1p_expr_t *base;
    asn1p_expr_t *comp;
    int dots = 0;

    if(xr_resolved(ref) || ref->comp_count < 1) return;
    name = ref->components[0].name;
    if(*name == '@') name++;
    while(*name == '.') {
        dots++;
        name++;
    }
    base = xr_at_parent(x, dots);
    comp = xr_member(base, name);
    for(size_t i = 1; comp && i < ref->comp_count; i++)
        comp = xr_member(xr_terminal(x, comp), ref->components[i].name);
    if(comp) {
        ref->ref_expr = comp;
    } else if(base && x->generic && xr_is_dummy_type(x, base)) {
        xr_dummy(x, ref);
    } else {
        xr_unresolved(x, ref, "component relation");
    }
}

/* WITH COMPONENTS: identifiers name components of the constrained type. */
static void
xr_with_components(xref_t *x, asn1p_constraint_t *ct, asn1p_expr_t *type) {
    for(unsigned int i = 0; i < ct->el_count; i++) {
        asn1p_constraint_t *el = ct->elements[i];
        asn1p_expr_t *comp = NULL;
        if(el->type == ACT_EL_EXT) continue; /* PartialSpecification */
        if(el->type == ACT_EL_VALUE && el->value
           && el->value->type == ATV_REFERENCED
           && el->value->value.reference->comp_count == 1) {
            asn1p_ref_t *ref = el->value->value.reference;
            comp = xr_component_ref(x, ref, type, ref->components[0].name,
                                    "component in WITH COMPONENTS");
            for(unsigned int k = 0; k < el->el_count; k++)
                xr_constraint(x, el->elements[k], comp);
        } else {
            xr_constraint(x, el, type);
        }
    }
}

static void
xr_constraint(xref_t *x, asn1p_constraint_t *ct, asn1p_expr_t *type) {
    if(!ct) return;
    switch(ct->type) {
    case ACT_CT_WCOMPS:
        xr_with_components(x, ct, type);
        return;
    case ACT_CT_WCOMP: {
        /* WITH COMPONENT: the constraint applies to the element type */
        asn1p_expr_t *term = xr_terminal(x, type);
        asn1p_expr_t *elem = term ? TQ_FIRST(&(term->members)) : NULL;
        for(unsigned int i = 0; i < ct->el_count; i++)
            xr_constraint(x, ct->elements[i], elem);
        return;
    }
    case ACT_CA_CRC:
        /* {ObjectSet}{@a}{@.b}: the object set, then component relations */
        for(unsigned int i = 0; i < ct->el_count; i++)
            xr_constraint(x, ct->elements[i], i ? NULL : type);
        return;
    case ACT_CT_SIZE:
        type = NULL; /* Sizes are integers: no named numbers */
        break;
    default:
        break;
    }
    xr_value(x, ct->containedSubtype, type);
    xr_value(x, ct->inlined_subtype, type);
    xr_value(x, ct->value, type);
    xr_value(x, ct->range_start, type);
    xr_value(x, ct->range_stop, type);
    for(unsigned int i = 0; i < ct->el_count; i++)
        xr_constraint(x, ct->elements[i], type);
}

static void
xr_value(xref_t *x, asn1p_value_t *v, asn1p_expr_t *type) {
    if(!v) return;
    switch(v->type) {
    case ATV_REFERENCED: {
        asn1p_ref_t *ref = v->value.reference;
        if(!ref || xr_resolved(ref) || ref->comp_count < 1) return;
        switch(ref->components[0].lex_type) {
        case RLT_Atlowercase:
        case RLT_AtDotlowercase:
            xr_at_ref(x, ref);
            return;
        case RLT_lowercase:
            if(ref->comp_count == 1 && !xr_dummy(x, ref)) {
                /* A named number, enumeration, or named bit of the type */
                asn1p_expr_t *m = xr_member(xr_terminal(x, type),
                                            ref->components[0].name);
                if(m && m->expr_type == A1TC_UNIVERVAL) {
                    ref->ref_expr = m;
                    return;
                }
            }
            break;
        default:
            break;
        }
        xr_ref(x, ref, "value reference");
        return;
    }
    case ATV_TYPE:
        xr_expr(x, v->value.v_type);
        return;
    case ATV_VALUESET:
        xr_constraint(x, v->value.constraint, type);
        return;
    case ATV_CHOICE_IDENTIFIER: {
        asn1p_expr_t *alt = xr_member(xr_terminal(x, type),
                                      v->value.choice_identifier.identifier);
        xr_value(x, v->value.choice_identifier.value, alt);
        return;
    }
    default:
        return;
    }
}

static void
xr_members(xref_t *x, asn1p_expr_t *e) {
    asn1p_expr_t *m;
    int structured = e->expr_type == ASN_CONSTR_SEQUENCE
                     || e->expr_type == ASN_CONSTR_SET
                     || e->expr_type == ASN_CONSTR_CHOICE
                     || e->expr_type == ASN_CONSTR_SEQUENCE_OF
                     || e->expr_type == ASN_CONSTR_SET_OF;
    /* An extension addition group is not a construction (X.682 10.10) */
    int push = structured && !(e->ext_group > 0 && !e->Identifier);
    if(push) {
        if(x->nouter >= XREF_MAX_DEPTH) return;
        x->outer[x->nouter++] = e;
    }
    TQ_FOR(m, &(e->members), next) xr_expr(x, m);
    if(push) x->nouter--;
}

static void
xr_expr(xref_t *x, asn1p_expr_t *e) {
    asn1p_expr_t *saved_generic = x->generic;
    int saved_obase = x->obase;

    if(!e) return;
    for(int i = 0; i < x->depth; i++)
        if(x->visiting[i] == e) return; /* Cycle */
    if(x->depth >= XREF_MAX_DEPTH) return;
    x->visiting[x->depth++] = e;

    if(e->lhs_params && e->spec_index == -1) {
        x->generic = e; /* Generic form: DummyReferences are in scope */
    } else if(e->spec_index >= 0) {
        x->generic = NULL;     /* Specialization: dummies are substituted */
        x->obase = x->nouter;  /* A specialization is an outermost type */
    }
    if(e->lhs_params) {
        for(int i = 0; i < e->lhs_params->params_count; i++) {
            asn1p_ref_t *gov = e->lhs_params->params[i].governor;
            if(gov) xr_ref(x, gov, "parameter governor");
        }
    }

    xr_ref_args(x, e->reference, e->rhs_pspecs, "reference");
    if(e->rhs_pspecs) {
        asn1p_expr_t *m;
        TQ_FOR(m, &(e->rhs_pspecs->members), next) xr_expr(x, m);
    }
    xr_constraint(x, e->constraints, e);
    xr_constraint(x, e->combined_constraints, e);
    xr_value(x, e->value, e);
    xr_value(x, e->marker.default_value, e);
    for(int i = 0; i < e->specializations.pspecs_count; i++) {
        struct asn1p_pspec_s *ps = &e->specializations.pspec[i];
        if(ps->rhs_pspecs) {
            asn1p_expr_t *m;
            x->generic = NULL; /* Actual parameters of the referencing place */
            TQ_FOR(m, &(ps->rhs_pspecs->members), next) xr_expr(x, m);
            x->generic = (e->lhs_params && e->spec_index == -1) ? e : saved_generic;
        }
        xr_expr(x, ps->my_clone);
    }
    if(e->ioc_table) {
        for(size_t r = 0; r < e->ioc_table->rows; r++)
            for(size_t c = 0; c < e->ioc_table->row[r]->columns; c++)
                xr_expr(x, e->ioc_table->row[r]->column[c].value);
    }
    xr_members(x, e);

    x->depth--;
    x->generic = saved_generic;
    x->obase = saved_obase;
}

int
asn1f_resolve_all_references(arg_t *arg) {
    xref_t x;
    asn1p_expr_t *expr;

    memset(&x, 0, sizeof(x));
    x.arg = arg;
    x.look = *arg;
    x.look.eh = NULL; /* Quiet: this pass reports its own diagnostics */
    x.look.debug = NULL;

    TQ_FOR(expr, &(arg->mod->members), next) {
        if(expr->_mark & TM_ENCODING_INSTRUCTION) continue;
        arg->expr = expr;
        x.top = expr;
        xr_expr(&x, expr);
    }

    return x.failed ? -1 : 0;
}
