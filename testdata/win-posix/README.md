Windows-only stand-ins for the POSIX headers the astlint fixtures include
(`<unistd.h>`, `<strings.h>`, `<pthread.h>`). The UCRT has none of them, so on
Windows tests/cpp/test_astlint.cpp copies these next to the fixture (the AST
front end puts the fixture's directory on the include path). On Linux and
macOS the fixtures parse against the real system headers and these files are
never used. Declarations only: astlint reads call names and types, nothing
here is linked or run.
