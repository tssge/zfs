# OpenZFS (ZFS)

OpenZFS is an advanced file system and volume manager originally developed for Solaris and now maintained by the OpenZFS community. This repository contains the code for running OpenZFS on Linux and FreeBSD.

Always reference these instructions first and fallback to search or bash commands only when you encounter unexpected information that does not match the info here.

## Working Effectively

- Bootstrap, build, and test the repository:
  - `sudo apt-get update`
  - `sudo apt-get install -y build-essential autoconf automake libtool git alien fakeroot gawk uuid-dev libuuid1 libblkid-dev libssl-dev zlib1g-dev libaio-dev libattr1-dev libelf-dev python3-dev python3-setuptools python3-cffi libffi-dev python3-packaging dkms libtirpc-dev linux-headers-$(uname -r)`
  - `./autogen.sh` -- takes 15 seconds. NEVER CANCEL.
  - `./configure` -- takes 70 seconds. NEVER CANCEL. Set timeout to 5+ minutes.
  - `make -j$(nproc)` -- takes 3-4 minutes. NEVER CANCEL. Set timeout to 10+ minutes.
- Run tests:
  - Individual test: `sudo ./scripts/zfs-tests.sh -vxckf -t <test_path>`
  - Test runner: `./tests/test-runner/bin/test-runner.py --help` for options
  - Full test suite: `sudo ./scripts/zfs-tests.sh` -- takes hours. NEVER CANCEL. Set timeout to 180+ minutes.
- Code quality checks:
  - `sudo apt-get install -y cppcheck shellcheck devscripts mandoc python3-flake8`
  - `make checkstyle` -- takes 15 seconds. NEVER CANCEL.
  - `make lint` -- may fail in some environments due to ISA detection issues
- Install additional tools if needed:
  - Style checking tools: `sudo apt-get install -y devscripts mandoc python3-flake8`
  - Debugging tools: As needed for development

## Validation

- ALWAYS manually validate any kernel module changes require root privileges and may not work in all environments.
- You can build the ZFS utilities and libraries, however kernel module loading may fail in containerized environments.
- Test ZFS commands: `sudo ./zfs list` or `sudo ./zpool status` (expect "Permission denied" if not root, or "no pools available" if no ZFS pools exist).
- ALWAYS run through at least one complete build cycle after making changes.
- The codebase includes comprehensive test suites, but many tests require root privileges and specific hardware setups.
- Always run `make checkstyle` before committing changes or the CI (.github/workflows/checkstyle.yaml) will fail.

## Critical Build Requirements

- **NEVER CANCEL BUILDS**: autogen.sh takes ~15s, configure takes ~70s, make takes ~4min. Set timeouts accordingly.
- **Dependencies are critical**: libtirpc-dev is required and often missing. Install all dependencies before building.
- **Root privileges required**: Many operations require sudo, especially testing and module operations.
- **Kernel headers required**: linux-headers-$(uname -r) must be installed for kernel module compilation.

## Common Tasks

The following are outputs from frequently run commands. Reference them instead of viewing, searching, or running bash commands to save time.

### Repository Root
```
ls -la
AUTHORS               cmd/                  include/              rpm/
CODE_OF_CONDUCT.md    config/               lib/                  scripts/
COPYRIGHT             configure.ac          LICENSE               tests/
LICENSE               contrib/              Makefile.am           udev/
META                  copy-builtin          man/                  zfs.release.in
NEWS                  etc/                  module/
NOTICE                .git/                 .github/
README.md             .gitignore            autogen.sh
RELEASES.md           .gitmodules           .editorconfig
TEST                  .mailmap              .gitattributes
```

### Key Build Files
- `configure.ac`: Main autotools configuration
- `Makefile.am`: Top-level Makefile
- `autogen.sh`: Generates configure script
- `scripts/`: Build and test utilities
- `tests/`: Comprehensive test suite

### Key Directories
- `cmd/`: User-space utilities (zfs, zpool, zdb, etc.)
- `lib/`: Libraries (libzfs, libzpool, etc.)
- `module/`: Kernel modules (ZFS and SPL)
- `include/`: Header files
- `tests/`: Test framework and test cases
- `man/`: Manual pages
- `.github/workflows/`: CI/CD configuration

### Dependencies for Ubuntu/Debian
```bash
sudo apt-get install -y \
  build-essential autoconf automake libtool git alien fakeroot gawk \
  uuid-dev libuuid1 libblkid-dev libssl-dev zlib1g-dev libaio-dev \
  libattr1-dev libelf-dev python3-dev python3-setuptools python3-cffi \
  libffi-dev python3-packaging dkms libtirpc-dev linux-headers-$(uname -r) \
  cppcheck shellcheck devscripts mandoc python3-flake8
```

### Build Process
```bash
# Generate configure script
./autogen.sh

# Configure build (detects system and dependencies)
./configure

# Build everything (userspace + kernel modules)
make -j$(nproc)

# Run style checks
make checkstyle

# Run static analysis (may fail in some environments)
make lint
```

### Testing
```bash
# Test individual component
sudo ./scripts/zfs-tests.sh -vxckf -t tests/functional/alloc_class/alloc_class_001_pos

# Run test runner directly
./tests/test-runner/bin/test-runner.py -h

# Full test suite (requires root, takes hours)
sudo ./scripts/zfs-tests.sh

# Check ZFS commands work
sudo ./zfs list
sudo ./zpool status
```

### GitHub Actions Workflows
- `checkstyle.yaml`: Style and lint checking
- `zfs-qemu.yml`: Main testing on multiple OS platforms
- `zloop.yml`: Stress testing with zloop.sh
- `codeql.yml`: Security analysis

### Important Notes
- ZFS requires root privileges for most operations
- Kernel module compilation requires matching kernel headers
- Tests may require specific disk setups and root access
- Build times: autogen (~15s), configure (~70s), make (~4min)
- The codebase supports both Linux and FreeBSD
- Follow C Style and Coding Standards for SunOS
- Always use `Signed-off-by` in commit messages
- CI runs on multiple platforms with different test scopes