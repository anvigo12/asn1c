#ifndef	ASN1FIX_PARAMETERIZATION_H
#define	ASN1FIX_PARAMETERIZATION_H

asn1p_expr_t *asn1f_parameterization_fork(arg_t *arg, asn1p_expr_t *expr, asn1p_expr_t *rhs_pspecs);

/*
 * Create the specialization of each parameterized type reference with
 * actual parameters inside the constraints and the actual parameters of
 * arg->expr (for tree printers, A1F_RESOLVE_ALL_REFS).
 */
int asn1f_specialize_nested(arg_t *arg);

#endif	/* ASN1FIX_PARAMETERIZATION_H */
