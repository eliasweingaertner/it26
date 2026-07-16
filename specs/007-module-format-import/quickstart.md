# Quickstart: verifying module import

- `ited testdata\import_test.s3m` (and .mod/.mtm/.669/.xm): loads,
  plays, shows patterns/samples; F10 saves as .IT; reloading the .IT
  plays equivalently (US3).
- F9 lists all five formats alongside .IT files.
- Wrong-extension files are sniffed by signature.
- `ITED_SELFTEST=1 ited testdata\itdemo.it` reports `IMPORT OK`
  (loads each generated test module, renders 2 s non-silent, then
  save→reload check).
- Gates: determinism ×4 + roundtrip ×4 unchanged.
- Regenerate test modules: `python tools/gen_import_tests.py testdata`.
