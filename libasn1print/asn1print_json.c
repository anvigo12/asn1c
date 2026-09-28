/*
 * Print the ASN.1 syntax tree as a JSON document (asn1c -E -print-json).
 * Document format name: "asn1c.ast.json".
 *
 * The document mirrors the libasn1parser data structures. Enumerated
 * values use the names of the C enumerators (for example
 * "ASN_CONSTR_SEQUENCE"). Object keys use lowerCamelCase. Values of
 * asn1c_integer_t are decimal strings, so that no precision is lost.
 * A key is omitted when its value is empty, zero, or false.
 *
 * Every expression has an "id" that is a path from its module:
 *   Module.Name                 top-level assignment
 *   <parent>/member             named member
 *   <parent>/#<n>               unnamed member or extension marker
 *                               (0-based position among the members)
 *   <parent>{<n>}               actual parameter <n> of a parameterized
 *                               type reference
 *   <parent>@<n>                specialization <n> of a parameterized type
 *   <parent>^<n>{<k>}           actual parameter <k> recorded for
 *                               specialization <n>
 *   <parent>$<n>                expression inside a value or constraint
 *                               (<n> counts in print order)
 *   <parent>!<row>.<column>     value of an information object table cell
 * With -F, references have a "resolvedId" when the fixer resolved them.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <assert.h>

#include <asn1_buffer.h>
#include <asn1parser.h>

#include "asn1print.h"

#define JSON_FORMAT_NAME "asn1c.ast.json"
#define JSON_FORMAT_VERSION 1
#define JSON_MAX_DEPTH 512

/*
 * Minimal streaming JSON writer.
 */
static struct {
    int depth;
    int count[JSON_MAX_DEPTH]; /* Values written at each depth */
    int after_key;             /* Next value follows a key */
    int pretty;
    int failed;                /* Output error or depth overflow */
} jw;

static void
j_write(const char *s, size_t len) {
    if(len && fwrite(s, 1, len, stdout) != len) jw.failed = 1;
}

static void
j_puts(const char *s) {
    j_write(s, strlen(s));
}

static void
j_newline(void) {
    if(jw.pretty) {
        j_write("\n", 1);
        for(int i = 0; i < jw.depth; i++) j_write("  ", 2);
    }
}

/* Separator and indentation before a value. */
static void
j_value_prefix(void) {
    if(jw.after_key) {
        jw.after_key = 0;
        return;
    }
    if(jw.depth > 0) {
        if(jw.count[jw.depth]++) j_write(",", 1);
        j_newline();
    }
}

static void
j_string_body(const unsigned char *s, size_t len) {
    static const char hex[] = "0123456789abcdef";
    j_write("\"", 1);
    for(size_t i = 0; i < len;) {
        unsigned char c = s[i];
        size_t n = 0; /* Length of a valid UTF-8 sequence at s[i] */
        if(c < 0x80) {
            n = 1;
        } else if(c >= 0xc2 && c <= 0xdf) {
            n = 2;
        } else if(c >= 0xe0 && c <= 0xef) {
            n = 3;
        } else if(c >= 0xf0 && c <= 0xf4) {
            n = 4;
        }
        if(n > 1) {
            if(i + n > len) {
                n = 0;
            } else {
                for(size_t k = 1; k < n; k++)
                    if((s[i + k] & 0xc0) != 0x80) n = 0;
            }
        }
        if(n == 1) {
            switch(c) {
            case '"': j_puts("\\\""); break;
            case '\\': j_puts("\\\\"); break;
            case '\n': j_puts("\\n"); break;
            case '\r': j_puts("\\r"); break;
            case '\t': j_puts("\\t"); break;
            default:
                if(c < 0x20) {
                    char buf[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0};
                    j_puts(buf);
                } else {
                    j_write((const char *)&s[i], 1);
                }
            }
            i++;
        } else if(n > 1) {
            j_write((const char *)&s[i], n);
            i += n;
        } else {
            /* Not UTF-8: map the byte to the code point of equal value. */
            char buf[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0};
            j_puts(buf);
            i++;
        }
    }
    j_write("\"", 1);
}

static void
j_begin(char open) {
    j_value_prefix();
    j_write(&open, 1);
    if(jw.depth + 1 >= JSON_MAX_DEPTH) {
        jw.failed = 1;
        return;
    }
    jw.depth++;
    jw.count[jw.depth] = 0;
}

static void
j_end(char close) {
    int had_values = jw.count[jw.depth];
    if(jw.depth > 0) jw.depth--;
    if(had_values) j_newline();
    j_write(&close, 1);
}

#define j_obj_begin() j_begin('{')
#define j_obj_end() j_end('}')
#define j_arr_begin() j_begin('[')
#define j_arr_end() j_end(']')

static void
j_key(const char *key) {
    if(jw.count[jw.depth]++) j_write(",", 1);
    j_newline();
    j_string_body((const unsigned char *)key, strlen(key));
    j_write(jw.pretty ? ": " : ":", jw.pretty ? 2 : 1);
    jw.after_key = 1;
}

static void
j_str(const char *s) {
    j_value_prefix();
    j_string_body((const unsigned char *)s, strlen(s));
}

static void
j_strn(const unsigned char *s, size_t len) {
    j_value_prefix();
    j_string_body(s, len);
}

static void
j_int(long v) {
    char buf[32];
    j_value_prefix();
    snprintf(buf, sizeof(buf), "%ld", v);
    j_puts(buf);
}

static void
j_bool(int v) {
    j_value_prefix();
    j_puts(v ? "true" : "false");
}

static void
j_double(double v) {
    char buf[64];
    j_value_prefix();
    if(isnan(v)) {
        j_puts("\"NaN\"");
    } else if(isinf(v)) {
        j_puts(v > 0 ? "\"Infinity\"" : "\"-Infinity\"");
    } else {
        snprintf(buf, sizeof(buf), "%.17g", v);
        j_puts(buf);
    }
}

/* Key and value helpers that omit empty values. */
static void
jk_str(const char *key, const char *s) {
    if(s && *s) {
        j_key(key);
        j_str(s);
    }
}

static void
jk_int(const char *key, long v) {
    if(v) {
        j_key(key);
        j_int(v);
    }
}

static void
jk_bool(const char *key, int v) {
    if(v) {
        j_key(key);
        j_bool(1);
    }
}

static void
jk_integer(const char *key, asn1c_integer_t v) {
    j_key(key);
    j_str(asn1p_itoa(v));
}

/*
 * Names of the C enumerators.
 */
#define CASE_NAME(x) case x: return #x

static const char *
meta_type_name(asn1p_expr_meta_e t) {
    switch(t) {
    CASE_NAME(AMT_INVALID);
    CASE_NAME(AMT_TYPE);
    CASE_NAME(AMT_TYPEREF);
    CASE_NAME(AMT_VALUE);
    CASE_NAME(AMT_VALUESET);
    CASE_NAME(AMT_OBJECT);
    CASE_NAME(AMT_OBJECTCLASS);
    CASE_NAME(AMT_OBJECTFIELD);
    CASE_NAME(AMT_EXPR_META_MAX);
    }
    return "AMT_INVALID";
}

static const char *
expr_type_name(asn1p_expr_type_e t) {
    switch(t) {
    CASE_NAME(A1TC_INVALID);
    CASE_NAME(A1TC_REFERENCE);
    CASE_NAME(A1TC_EXPORTVAR);
    CASE_NAME(A1TC_UNIVERVAL);
    CASE_NAME(A1TC_BITVECTOR);
    CASE_NAME(A1TC_OPAQUE);
    CASE_NAME(A1TC_EXTENSIBLE);
    CASE_NAME(A1TC_COMPONENTS_OF);
    CASE_NAME(A1TC_VALUESET);
    CASE_NAME(A1TC_CLASSDEF);
    CASE_NAME(A1TC_INSTANCE);
    CASE_NAME(A1TC_CLASSFIELD_TFS);
    CASE_NAME(A1TC_CLASSFIELD_FTVFS);
    CASE_NAME(A1TC_CLASSFIELD_VTVFS);
    CASE_NAME(A1TC_CLASSFIELD_FTVSFS);
    CASE_NAME(A1TC_CLASSFIELD_VTVSFS);
    CASE_NAME(A1TC_CLASSFIELD_OFS);
    CASE_NAME(A1TC_CLASSFIELD_OSFS);
    CASE_NAME(ASN_CONSTR_SEQUENCE);
    CASE_NAME(ASN_CONSTR_CHOICE);
    CASE_NAME(ASN_CONSTR_SET);
    CASE_NAME(ASN_CONSTR_SEQUENCE_OF);
    CASE_NAME(ASN_CONSTR_SET_OF);
    CASE_NAME(ASN_CONSTR_OPEN_TYPE);
    CASE_NAME(ASN_TYPE_ANY);
    CASE_NAME(ASN_BASIC_BOOLEAN);
    CASE_NAME(ASN_BASIC_NULL);
    CASE_NAME(ASN_BASIC_INTEGER);
    CASE_NAME(ASN_BASIC_REAL);
    CASE_NAME(ASN_BASIC_ENUMERATED);
    CASE_NAME(ASN_BASIC_BIT_STRING);
    CASE_NAME(ASN_BASIC_OCTET_STRING);
    CASE_NAME(ASN_BASIC_OBJECT_IDENTIFIER);
    CASE_NAME(ASN_BASIC_RELATIVE_OID);
    CASE_NAME(ASN_BASIC_EXTERNAL);
    CASE_NAME(ASN_BASIC_EMBEDDED_PDV);
    CASE_NAME(ASN_BASIC_CHARACTER_STRING);
    CASE_NAME(ASN_BASIC_UTCTime);
    CASE_NAME(ASN_BASIC_GeneralizedTime);
    CASE_NAME(ASN_STRING_IA5String);
    CASE_NAME(ASN_STRING_PrintableString);
    CASE_NAME(ASN_STRING_VisibleString);
    CASE_NAME(ASN_STRING_ISO646String);
    CASE_NAME(ASN_STRING_NumericString);
    CASE_NAME(ASN_STRING_UniversalString);
    CASE_NAME(ASN_STRING_BMPString);
    CASE_NAME(ASN_STRING_UTF8String);
    CASE_NAME(ASN_STRING_GeneralString);
    CASE_NAME(ASN_STRING_GraphicString);
    CASE_NAME(ASN_STRING_TeletexString);
    CASE_NAME(ASN_STRING_T61String);
    CASE_NAME(ASN_STRING_VideotexString);
    CASE_NAME(ASN_STRING_ObjectDescriptor);
    CASE_NAME(ASN_EXPR_TYPE_MAX);
    }
    return "A1TC_INVALID";
}

static const char *
constraint_type_name(enum asn1p_constraint_type_e t) {
    switch(t) {
    CASE_NAME(ACT_INVALID);
    CASE_NAME(ACT_EL_TYPE);
    CASE_NAME(ACT_EL_VALUE);
    CASE_NAME(ACT_EL_RANGE);
    CASE_NAME(ACT_EL_LLRANGE);
    CASE_NAME(ACT_EL_RLRANGE);
    CASE_NAME(ACT_EL_ULRANGE);
    CASE_NAME(ACT_EL_EXT);
    CASE_NAME(ACT_CT_SIZE);
    CASE_NAME(ACT_CT_FROM);
    CASE_NAME(ACT_CT_WCOMP);
    CASE_NAME(ACT_CT_WCOMPS);
    CASE_NAME(ACT_CT_CTDBY);
    CASE_NAME(ACT_CT_CTNG);
    CASE_NAME(ACT_CT_PATTERN);
    CASE_NAME(ACT_CA_SET);
    CASE_NAME(ACT_CA_CRC);
    CASE_NAME(ACT_CA_CSV);
    CASE_NAME(ACT_CA_UNI);
    CASE_NAME(ACT_CA_INT);
    CASE_NAME(ACT_CA_EXC);
    CASE_NAME(ACT_CA_AEX);
    }
    return "ACT_INVALID";
}

static const char *
presence_name(enum asn1p_constr_pres_e p) {
    switch(p) {
    CASE_NAME(ACPRES_DEFAULT);
    CASE_NAME(ACPRES_PRESENT);
    CASE_NAME(ACPRES_ABSENT);
    CASE_NAME(ACPRES_OPTIONAL);
    }
    return "ACPRES_DEFAULT";
}

static const char *
value_type_name(int t) {
    switch(t) {
    CASE_NAME(ATV_NOVALUE);
    CASE_NAME(ATV_TYPE);
    CASE_NAME(ATV_NULL);
    CASE_NAME(ATV_REAL);
    CASE_NAME(ATV_INTEGER);
    CASE_NAME(ATV_MAX);
    CASE_NAME(ATV_MIN);
    CASE_NAME(ATV_TRUE);
    CASE_NAME(ATV_FALSE);
    CASE_NAME(ATV_TUPLE);
    CASE_NAME(ATV_QUADRUPLE);
    CASE_NAME(ATV_STRING);
    CASE_NAME(ATV_UNPARSED);
    CASE_NAME(ATV_BITVECTOR);
    CASE_NAME(ATV_VALUESET);
    CASE_NAME(ATV_REFERENCED);
    CASE_NAME(ATV_CHOICE_IDENTIFIER);
    }
    return "ATV_NOVALUE";
}

static const char *
ref_lex_type_name(enum asn1p_ref_lex_type_e t) {
    switch(t) {
    CASE_NAME(RLT_UNKNOWN);
    CASE_NAME(RLT_CAPITALS);
    CASE_NAME(RLT_Uppercase);
    CASE_NAME(RLT_lowercase);
    CASE_NAME(RLT_AmpUppercase);
    CASE_NAME(RLT_Amplowercase);
    CASE_NAME(RLT_Atlowercase);
    CASE_NAME(RLT_AtDotlowercase);
    CASE_NAME(RLT_MAX);
    }
    return "RLT_UNKNOWN";
}

static const char *
tag_class_name(int c) {
    switch(c) {
    case TC_NOCLASS: return "TC_NOCLASS";
    case TC_UNIVERSAL: return "TC_UNIVERSAL";
    case TC_APPLICATION: return "TC_APPLICATION";
    case TC_CONTEXT_SPECIFIC: return "TC_CONTEXT_SPECIFIC";
    case TC_PRIVATE: return "TC_PRIVATE";
    }
    return "TC_NOCLASS";
}

static const char *
tag_mode_name(int m) {
    switch(m) {
    case TM_DEFAULT: return "TM_DEFAULT";
    case TM_IMPLICIT: return "TM_IMPLICIT";
    case TM_EXPLICIT: return "TM_EXPLICIT";
    }
    return "TM_DEFAULT";
}

static const char *
wsyntx_chunk_name(int t) {
    switch(t) {
    case WC_LITERAL: return "WC_LITERAL";
    case WC_WHITESPACE: return "WC_WHITESPACE";
    case WC_FIELD: return "WC_FIELD";
    case WC_OPTIONALGROUP: return "WC_OPTIONALGROUP";
    }
    return "WC_LITERAL";
}

static const char *
encoding_control_name(enum asn1p_encoding_control_type_e t) {
    switch(t) {
    CASE_NAME(EC_NONE);
    CASE_NAME(EC_XER_HEXADECIMAL);
    CASE_NAME(EC_XER_BASE64);
    CASE_NAME(EC_XER_UTF8);
    CASE_NAME(EC_XER_TEXT);
    CASE_NAME(EC_XER_DECIMAL);
    CASE_NAME(EC_XER_GLOBAL_DEFAULTS_MODIFIED_ENCODINGS);
    CASE_NAME(EC_JER_BASE64);
    CASE_NAME(EC_JER_TEXT);
    CASE_NAME(EC_JER_NAME);
    }
    return "EC_NONE";
}

/*
 * Expression identifiers.
 */
static char *
str_printf(const char *fmt, ...) {
    va_list ap;
    int len;
    char *s;

    va_start(ap, fmt);
    len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    s = malloc(len + 1);
    assert(s);
    va_start(ap, fmt);
    vsnprintf(s, len + 1, fmt, ap);
    va_end(ap);
    return s;
}

static int
is_positional(const asn1p_expr_t *e) {
    return e->Identifier == NULL || e->expr_type == A1TC_EXTENSIBLE;
}

/*
 * Identifiers of specialization clones. The fixer names a clone after its
 * actual parameter, so the identifier comes from the parameterized type:
 * "<Module>.<Type>@<n>", as printed under "specializations".
 */
static struct {
    const asn1p_expr_t **expr;
    char **id;
    size_t count;
    size_t size;
} clones;

static void
clones_add(const asn1p_expr_t *e, char *id) {
    if(clones.count == clones.size) {
        clones.size = clones.size ? 2 * clones.size : 64;
        clones.expr = realloc(clones.expr, clones.size * sizeof(clones.expr[0]));
        clones.id = realloc(clones.id, clones.size * sizeof(clones.id[0]));
        assert(clones.expr && clones.id);
    }
    clones.expr[clones.count] = e;
    clones.id[clones.count] = id;
    clones.count++;
}

static void
clones_collect(asn1p_t *asn) {
    asn1p_module_t *mod;
    asn1p_expr_t *tc;
    TQ_FOR(mod, &(asn->modules), mod_next) {
        if(mod->_tags & MT_STANDARD_MODULE) break;
        TQ_FOR(tc, &(mod->members), next) {
            for(int i = 0; tc->Identifier && i < tc->specializations.pspecs_count; i++) {
                if(tc->specializations.pspec[i].my_clone)
                    clones_add(tc->specializations.pspec[i].my_clone,
                               str_printf("%s.%s@%d", mod->ModuleName, tc->Identifier, i));
            }
        }
    }
}

static void
clones_free(void) {
    for(size_t i = 0; i < clones.count; i++) free(clones.id[i]);
    free(clones.expr);
    free(clones.id);
    memset(&clones, 0, sizeof(clones));
}

/* Path of an expression, from its parent chain. NULL if unknown. */
static char *
expr_path(const asn1p_expr_t *e, int depth) {
    if(e == NULL || depth > 64) return NULL;
    if(e->parent_expr == NULL) {
        for(size_t i = 0; i < clones.count; i++)
            if(clones.expr[i] == e) return strdup(clones.id[i]);
        if(!e->module || !e->Identifier) return NULL;
        if(e->spec_index >= 0)
            return str_printf("%s.%s@%d", e->module->ModuleName,
                              e->Identifier, e->spec_index);
        return str_printf("%s.%s", e->module->ModuleName, e->Identifier);
    } else {
        char *parent = expr_path(e->parent_expr, depth + 1);
        char *path;
        if(parent == NULL) return NULL;
        if(is_positional(e)) {
            const asn1p_expr_t *m;
            int n = 0;
            TQ_FOR(m, &(e->parent_expr->members), next) {
                if(m == e) break;
                n++;
            }
            path = str_printf("%s/#%d", parent, n);
        } else {
            path = str_printf("%s/%s", parent, e->Identifier);
        }
        free(parent);
        return path;
    }
}

/*
 * Printer state for one expression tree.
 */
static struct {
    const asn1p_expr_t *expr[JSON_MAX_DEPTH];
    const char *id[JSON_MAX_DEPTH];
    int embedded[JSON_MAX_DEPTH]; /* "$<n>" counter of each expression */
    int depth;
} stack;

static void j_expr(const asn1p_expr_t *e, const char *id);
static void j_value(const asn1p_value_t *v);
static void j_constraint(const asn1p_constraint_t *ct);

/* Identifier for an expression inside a value or constraint. */
static char *
embedded_id(void) {
    if(stack.depth == 0) return strdup("$");
    return str_printf("%s$%d", stack.id[stack.depth - 1],
                      stack.embedded[stack.depth - 1]++);
}

static void
j_oid(const asn1p_oid_t *oid) {
    j_obj_begin();
    j_key("arcs");
    j_arr_begin();
    for(int i = 0; i < oid->arcs_count; i++) {
        j_obj_begin();
        jk_str("name", oid->arcs[i].name);
        if(oid->arcs[i].number >= 0)
            jk_integer("number", oid->arcs[i].number);
        j_obj_end();
    }
    j_arr_end();
    j_obj_end();
}

static void
j_ref(const asn1p_ref_t *ref) {
    j_obj_begin();
    j_key("components");
    j_arr_begin();
    for(size_t i = 0; i < ref->comp_count; i++) {
        j_obj_begin();
        j_key("lexType");
        j_str(ref_lex_type_name(ref->components[i].lex_type));
        jk_str("name", ref->components[i].name);
        j_obj_end();
    }
    j_arr_end();
    jk_int("line", ref->_lineno);
    if(ref->module) jk_str("module", ref->module->ModuleName);
    if(ref->ref_expr) {
        char *rid = expr_path(ref->ref_expr, 0);
        jk_str("resolvedId", rid);
        free(rid);
    }
    j_obj_end();
}

static void
j_value(const asn1p_value_t *v) {
    j_obj_begin();
    j_key("kind");
    j_str(value_type_name(v->type));
    switch(v->type) {
    case ATV_NOVALUE:
    case ATV_NULL:
    case ATV_MAX:
    case ATV_MIN:
    case ATV_TRUE:
    case ATV_FALSE:
        break;
    case ATV_TYPE:
        if(v->value.v_type) {
            char *id = embedded_id();
            j_key("type");
            j_expr(v->value.v_type, id);
            free(id);
        }
        break;
    case ATV_REAL:
        j_key("real");
        j_double(v->value.v_double);
        break;
    case ATV_INTEGER:
        jk_integer("integer", v->value.v_integer);
        break;
    case ATV_TUPLE:
        j_key("tuple");
        j_arr_begin();
        j_int((long)(v->value.v_integer >> 4));
        j_int((long)(v->value.v_integer & 0x0f));
        j_arr_end();
        break;
    case ATV_QUADRUPLE:
        j_key("tuple");
        j_arr_begin();
        j_int((long)((v->value.v_integer >> 24) & 0xff));
        j_int((long)((v->value.v_integer >> 16) & 0xff));
        j_int((long)((v->value.v_integer >> 8) & 0xff));
        j_int((long)((v->value.v_integer >> 0) & 0xff));
        j_arr_end();
        break;
    case ATV_STRING:
    case ATV_UNPARSED:
        if(v->value.string.buf) {
            j_key("string");
            j_strn(v->value.string.buf, v->value.string.size);
        }
        break;
    case ATV_BITVECTOR: {
        int bits = v->value.binary_vector.size_in_bits;
        char *s = malloc(bits + 1);
        assert(s);
        for(int i = 0; i < bits; i++) {
            uint8_t uc = v->value.binary_vector.bits[i >> 3];
            s[i] = ((uc >> (7 - (i % 8))) & 1) ? '1' : '0';
        }
        s[bits] = '\0';
        j_key("bits");
        j_str(s);
        free(s);
        break;
    }
    case ATV_VALUESET:
        if(v->value.constraint) {
            j_key("constraint");
            j_constraint(v->value.constraint);
        }
        break;
    case ATV_REFERENCED:
        if(v->value.reference) {
            j_key("reference");
            j_ref(v->value.reference);
        }
        break;
    case ATV_CHOICE_IDENTIFIER:
        jk_str("choiceIdentifier", v->value.choice_identifier.identifier);
        if(v->value.choice_identifier.value) {
            j_key("choiceValue");
            j_value(v->value.choice_identifier.value);
        }
        break;
    }
    j_obj_end();
}

static void
j_constraint(const asn1p_constraint_t *ct) {
    j_obj_begin();
    j_key("kind");
    j_str(constraint_type_name(ct->type));
    if(ct->presence != ACPRES_DEFAULT) {
        j_key("presence");
        j_str(presence_name(ct->presence));
    }
    jk_int("line", ct->_lineno);
    if(ct->containedSubtype) {
        j_key("containedSubtype");
        j_value(ct->containedSubtype);
    }
    if(ct->value) {
        j_key("value");
        j_value(ct->value);
    }
    if(ct->range_start) {
        j_key("rangeStart");
        j_value(ct->range_start);
    }
    if(ct->range_stop) {
        j_key("rangeStop");
        j_value(ct->range_stop);
    }
    if(ct->el_count) {
        j_key("elements");
        j_arr_begin();
        for(unsigned int i = 0; i < ct->el_count; i++)
            j_constraint(ct->elements[i]);
        j_arr_end();
    }
    j_obj_end();
}

static void
j_with_syntax(const asn1p_wsyntx_t *wx) {
    const asn1p_wsyntx_chunk_t *wc;
    j_obj_begin();
    j_key("chunks");
    j_arr_begin();
    TQ_FOR(wc, &(wx->chunks), next) {
        j_obj_begin();
        j_key("kind");
        j_str(wsyntx_chunk_name(wc->type));
        if(wc->type == WC_OPTIONALGROUP) {
            if(wc->content.syntax) {
                j_key("group");
                j_with_syntax(wc->content.syntax);
            }
        } else {
            jk_str("token", wc->content.token);
        }
        j_obj_end();
    }
    j_arr_end();
    j_obj_end();
}

static void
j_ioc_table(const asn1p_ioc_table_t *it, const char *owner_id) {
    j_obj_begin();
    jk_bool("extensible", it->extensible);
    j_key("rows");
    j_arr_begin();
    for(size_t r = 0; r < it->rows; r++) {
        const asn1p_ioc_row_t *row = it->row[r];
        j_obj_begin();
        j_key("cells");
        j_arr_begin();
        for(size_t c = 0; c < row->columns; c++) {
            const struct asn1p_ioc_cell_s *cell = &row->column[c];
            j_obj_begin();
            if(cell->field) jk_str("field", cell->field->Identifier);
            if(cell->value) {
                char *id = str_printf("%s!%zu.%zu", owner_id, r, c);
                j_key("value");
                j_expr(cell->value, id);
                free(id);
            }
            jk_bool("newRef", cell->new_ref);
            j_obj_end();
        }
        j_arr_end();
        j_obj_end();
    }
    j_arr_end();
    j_obj_end();
}

static void
j_marker(const struct asn1p_expr_marker_s *m) {
    j_obj_begin();
    jk_bool("optional", (m->flags & EM_OPTIONAL) == EM_OPTIONAL);
    jk_bool("hasDefault", (m->flags & EM_DEFAULT) == EM_DEFAULT);
    jk_bool("indirect", (m->flags & EM_INDIRECT) != 0);
    jk_bool("omitable", (m->flags & EM_OMITABLE) != 0);
    jk_bool("unrecurse", (m->flags & EM_UNRECURSE) != 0);
    if(m->default_value) {
        j_key("defaultValue");
        j_value(m->default_value);
    }
    j_obj_end();
}

static void
j_members_array(const char *key, const asn1p_expr_t *container,
                const char *parent_id, int actual_params) {
    const asn1p_expr_t *m;
    int n = 0;
    j_key(key);
    j_arr_begin();
    TQ_FOR(m, &(container->members), next) {
        char *id;
        if(actual_params)
            id = str_printf("%s{%d}", parent_id, n);
        else if(is_positional(m))
            id = str_printf("%s/#%d", parent_id, n);
        else
            id = str_printf("%s/%s", parent_id, m->Identifier);
        j_expr(m, id);
        free(id);
        n++;
    }
    j_arr_end();
}

static void
j_expr(const asn1p_expr_t *e, const char *id) {
    j_obj_begin();
    jk_str("id", id);

    /* The same expression can be reached again through a cycle. */
    for(int i = 0; i < stack.depth; i++) {
        if(stack.expr[i] == e) {
            jk_str("identifier", e->Identifier);
            jk_str("cycleRef", stack.id[i]);
            j_obj_end();
            return;
        }
    }
    if(stack.depth >= JSON_MAX_DEPTH) {
        jw.failed = 1;
        j_obj_end();
        return;
    }
    stack.expr[stack.depth] = e;
    stack.id[stack.depth] = id;
    stack.embedded[stack.depth] = 0;
    stack.depth++;

    jk_str("identifier", e->Identifier);
    jk_int("line", e->_lineno);
    j_key("metaType");
    j_str(meta_type_name(e->meta_type));
    j_key("exprType");
    j_str(expr_type_name(e->expr_type));
    if(e->reference) {
        j_key("reference");
        j_ref(e->reference);
    }
    if(e->tag.tag_class != TC_NOCLASS || e->tag.tag_mode != TM_DEFAULT) {
        j_key("tag");
        j_obj_begin();
        j_key("tagClass");
        j_str(tag_class_name(e->tag.tag_class));
        if(e->tag.tag_mode != TM_DEFAULT) {
            j_key("tagMode");
            j_str(tag_mode_name(e->tag.tag_mode));
        }
        jk_integer("value", e->tag.tag_value);
        j_obj_end();
    }
    if(e->marker.flags) {
        j_key("marker");
        j_marker(&e->marker);
    }
    jk_bool("unique", e->unique);
    if(e->constraints) {
        j_key("constraints");
        j_constraint(e->constraints);
    }
    if(e->combined_constraints) {
        j_key("combinedConstraints");
        j_constraint(e->combined_constraints);
    }
    if(e->lhs_params && e->lhs_params->params_count) {
        j_key("lhsParams");
        j_arr_begin();
        for(int i = 0; i < e->lhs_params->params_count; i++) {
            const struct asn1p_param_s *p = &e->lhs_params->params[i];
            j_obj_begin();
            if(p->governor) {
                j_key("governor");
                j_ref(p->governor);
            }
            jk_str("argument", p->argument);
            j_obj_end();
        }
        j_arr_end();
    }
    if(e->rhs_pspecs) j_members_array("rhsPspecs", e->rhs_pspecs, id, 1);
    if(e->specializations.pspecs_count) {
        j_key("specializations");
        j_arr_begin();
        for(int i = 0; i < e->specializations.pspecs_count; i++) {
            const struct asn1p_pspec_s *ps = &e->specializations.pspec[i];
            char *aid = str_printf("%s^%d", id, i);
            char *sid = str_printf("%s@%d", id, i);
            j_obj_begin();
            if(ps->rhs_pspecs)
                j_members_array("rhsPspecs", ps->rhs_pspecs, aid, 1);
            if(ps->my_clone) {
                j_key("clone");
                j_expr(ps->my_clone, sid);
            }
            j_obj_end();
            free(aid);
            free(sid);
        }
        j_arr_end();
    }
    if(e->spec_index >= 0) {
        j_key("specIndex");
        j_int(e->spec_index);
    }
    if(e->value) {
        j_key("value");
        j_value(e->value);
    }
    if(e->with_syntax) {
        j_key("withSyntax");
        j_with_syntax(e->with_syntax);
    }
    if(e->ioc_table) {
        j_key("iocTable");
        j_ioc_table(e->ioc_table, id);
    }
    if(e->encoding_control.encoding_type != EC_NONE) {
        const struct asn1p_encoding_control_s *ec = &e->encoding_control;
        j_key("encodingControl");
        j_obj_begin();
        j_key("kind");
        j_str(encoding_control_name(ec->encoding_type));
        jk_str("reference", ec->encoding_reference);
        jk_str("targetPath", ec->target_path);
        jk_str("targetValue", ec->target_value);
        jk_str("replacement", ec->replacement);
        j_obj_end();
    }
    if(TQ_FIRST(&(e->members))) j_members_array("members", e, id, 0);

    stack.depth--;
    j_obj_end();
}

static void
j_xports_symbols(const asn1p_xports_t *xp) {
    const asn1p_expr_t *m;
    j_key("symbols");
    j_arr_begin();
    TQ_FOR(m, &(xp->xp_members), next) {
        j_obj_begin();
        jk_str("name", m->Identifier);
        jk_int("line", m->_lineno);
        j_obj_end();
    }
    j_arr_end();
}

static void
j_module(const asn1p_module_t *mod) {
    const asn1p_xports_t *xp;
    const asn1p_expr_t *tc;
    static const struct {
        asn1p_module_flags_e flag;
        const char *name;
    } flag_names[] = {
        {MSF_unk_INSTRUCTIONS, "MSF_unk_INSTRUCTIONS"},
        {MSF_TAG_INSTRUCTIONS, "MSF_TAG_INSTRUCTIONS"},
        {MSF_XER_INSTRUCTIONS, "MSF_XER_INSTRUCTIONS"},
        {MSF_JER_INSTRUCTIONS, "MSF_JER_INSTRUCTIONS"},
        {MSF_EXPLICIT_TAGS, "MSF_EXPLICIT_TAGS"},
        {MSF_IMPLICIT_TAGS, "MSF_IMPLICIT_TAGS"},
        {MSF_AUTOMATIC_TAGS, "MSF_AUTOMATIC_TAGS"},
        {MSF_EXTENSIBILITY_IMPLIED, "MSF_EXTENSIBILITY_IMPLIED"},
    };

    j_obj_begin();
    jk_str("name", mod->ModuleName);
    jk_str("sourceFile", mod->source_file_name);
    if(mod->module_oid) {
        j_key("oid");
        j_oid(mod->module_oid);
    }
    if(mod->module_flags) {
        j_key("flags");
        j_arr_begin();
        for(size_t i = 0; i < sizeof(flag_names) / sizeof(flag_names[0]); i++)
            if(mod->module_flags & flag_names[i].flag)
                j_str(flag_names[i].name);
        j_arr_end();
    }

    /* No "exports" key: no EXPORTS clause, or EXPORTS ALL. */
    xp = TQ_FIRST(&(mod->exports));
    if(xp) {
        j_key("exports");
        j_obj_begin();
        j_xports_symbols(xp);
        j_obj_end();
    }

    if(TQ_FIRST(&(mod->imports))) {
        j_key("imports");
        j_arr_begin();
        TQ_FOR(xp, &(mod->imports), xp_next) {
            j_obj_begin();
            jk_str("fromModule", xp->fromModuleName);
            if(xp->identifier.oid) {
                j_key("oid");
                j_oid(xp->identifier.oid);
            }
            if(xp->identifier.value) {
                j_key("assignedValue");
                j_value(xp->identifier.value);
            }
            if(xp->option == XPT_WITH_SUCCESSORS) {
                j_key("selectionOption");
                j_str("XPT_WITH_SUCCESSORS");
            } else if(xp->option == XPT_WITH_DESCENDANTS) {
                j_key("selectionOption");
                j_str("XPT_WITH_DESCENDANTS");
            }
            j_xports_symbols(xp);
            j_obj_end();
        }
        j_arr_end();
    }

    j_key("assignments");
    j_arr_begin();
    TQ_FOR(tc, &(mod->members), next) {
        char *id;
        /* Encoding instructions are not assignments (see asn1print). */
        if(tc->_mark & TM_ENCODING_INSTRUCTION) continue;
        if(tc->Identifier)
            id = str_printf("%s.%s", mod->ModuleName, tc->Identifier);
        else
            id = str_printf("%s.#", mod->ModuleName);
        j_expr(tc, id);
        free(id);
    }
    j_arr_end();
    j_obj_end();
}

int
asn1print_json(asn1p_t *asn, enum asn1print_flags flags) {
    asn1p_module_t *mod;

    if(asn == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(&jw, 0, sizeof(jw));
    memset(&stack, 0, sizeof(stack));
    jw.pretty = !(flags & APF_NOINDENT);
    clones_collect(asn);

    j_obj_begin();
    jk_str("format", JSON_FORMAT_NAME);
    jk_int("formatVersion", JSON_FORMAT_VERSION);
    jk_str("producer", "asn1c");
    jk_str("producerVersion", VERSION);
    j_key("view");
    j_str((flags & APF_FIXED_TREE) ? "RESOLVED" : "DECLARED");
    j_key("modules");
    j_arr_begin();
    TQ_FOR(mod, &(asn->modules), mod_next) {
        if(mod->_tags & MT_STANDARD_MODULE)
            break; /* Modules imported from skeletons come last */
        j_module(mod);
    }
    j_arr_end();
    j_obj_end();
    j_write("\n", 1);
    clones_free();

    if(fflush(stdout) != 0) jw.failed = 1;
    if(jw.failed) {
        errno = EIO;
        return -1;
    }
    return 0;
}
