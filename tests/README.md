# Test fixtures

Fixtures in this directory are intentionally synthetic and safe to version.
They are small enough to inspect manually and exist solely to establish stable
parser and numerical expectations.

`test_core.c` currently covers delimited spectrum loading, SPCAT catalogue
loading, binary searches, rolling averaging, and a basic isolated-peak case.
Future tests should add an explicit regression before a scientific bug is
fixed.
