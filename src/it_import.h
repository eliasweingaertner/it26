/*
 * it_import.h
 * -----------
 * Whole-module importers, ported from the original editor's
 * IT_D_RM.INC (D_LoadS3M / D_LoadXM / D_LoadMOD / D_LoadMTM /
 * D_Load669) and the pattern converters in PE_TRANS.INC.
 */

#ifndef IT_IMPORT_H
#define IT_IMPORT_H

/* Load any of the formats IT supported natively (.IT delegates to
 * Music_LoadIT). Format is sniffed by signature with a .MOD-style
 * fallback. Returns 1 on success. */
int Import_LoadModule(const char *path);

/* extension filter for the load requester */
int Import_KnownExt(const char *name);

#endif /* IT_IMPORT_H */
