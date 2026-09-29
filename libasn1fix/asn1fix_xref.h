#ifndef	ASN1FIX_XREF_H
#define	ASN1FIX_XREF_H

/*
 * Set the target of each reference of the current module that has none
 * (A1F_RESOLVE_ALL_REFS). Returns -1 if a reference cannot be resolved.
 */
int asn1f_resolve_all_references(arg_t *arg);

#endif	/* ASN1FIX_XREF_H */
