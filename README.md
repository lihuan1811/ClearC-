# C DiskGlow C++ Qt

This is the standalone C++/Qt migration of C DiskGlow.

The original Python/Flutter project remains in the sibling directory:

```text
../One-click-cleaning-of-C-drive
```

This repository contains only the C++/Qt application:

```text
src/
  main.cpp
  MainWindow.*
  CleanupEngine.*
  SystemCatalog.*
  AccountStore.*
```

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
```

The Windows GitHub Actions workflow installs Qt, builds the executable,
runs `windeployqt`, uploads `CDriveCleanerQt.zip`, and publishes a release.
