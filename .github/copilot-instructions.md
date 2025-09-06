# OpenZFS File System and Volume Manager

OpenZFS is an advanced file system and volume manager originally developed for Solaris and now maintained by the OpenZFS community for Linux and FreeBSD. This repository contains the code for running OpenZFS on Linux and FreeBSD.

Always reference these instructions first and fallback to search or bash commands only when you encounter unexpected information that does not match the info here.

## Working Effectively

### Build Dependencies and Setup
Install required build dependencies:
```bash
sudo apt-get update
sudo apt-get install -y build-essential autoconf automake libtool make gcc
sudo apt-get install -y linux-headers-$(uname -r) uuid-dev libattr1-dev libblkid-dev 
sudo apt-get install -y libssl-dev libaio-dev libelf-dev python3-dev python3-setuptools 
sudo apt-get install -y python3-cffi libffi-dev libtirpc-dev gettext
```

### Bootstrap, Build, and Test the Repository

**NEVER CANCEL any build or test commands** - builds take 1-2 minutes, full tests can take 1-2 hours. Set appropriate timeouts.

Build process for user-space utilities (RECOMMENDED for development):
```bash
./autogen.sh                                    # Takes ~12-15 seconds
./configure --with-config=user --enable-debug   # Takes ~6 seconds  
make -j$(nproc)                                 # Takes ~1.5-2 minutes. NEVER CANCEL. Set timeout to 10+ minutes.
```

Build process for complete system (user-space + kernel modules):
```bash
./autogen.sh                                    # Takes ~12-15 seconds
./configure --enable-debug                      # Takes ~1 minute 15 seconds
make -j$(nproc)                                 # Takes 2-5 minutes. NEVER CANCEL. Set timeout to 15+ minutes.
```

**WARNING**: The full kernel module build may fail on some systems due to kernel version incompatibilities or missing kernel build environment. Always try user-space build first for development work.

### Testing

Run code style and validation checks:
```bash
make checkstyle                                 # Takes ~30 seconds. NEVER CANCEL. Set timeout to 5+ minutes.
```

Test user-space functionality (requires root):
```bash
# Basic functionality test
sudo ./zfs version
sudo ./zpool version

# Run basic sanity tests (file-based, no real disks needed)
sudo ./scripts/zfs-tests.sh -f                 # Takes 25+ minutes. NEVER CANCEL. Set timeout to 60+ minutes.

# Run stress tests  
sudo ./scripts/zloop.sh -t 600                 # Runs for 10 minutes. NEVER CANCEL. Set timeout to 20+ minutes.
```

Run full test suite (WARNING: takes 1-2+ hours):
```bash
sudo ./scripts/zfs-tests.sh                    # Takes 1-2+ hours. NEVER CANCEL. Set timeout to 180+ minutes.
```

### Package Building

Build distribution packages (requires additional tools):
```bash
# For RPM-based distributions
make pkg-kmod pkg-utils

# For DEB-based distributions (requires alien)
sudo apt-get install -y alien
make pkg-utils
```

## Validation

### Manual Validation Requirements
- **ALWAYS** run the bootstrapping steps (autogen.sh -> configure -> make) first before testing any changes
- **ALWAYS** run `make checkstyle` before submitting changes or the CI will fail
- **CRITICAL**: For any kernel module changes, test with both user-space and kernel builds
- **MANUAL TESTING**: After making changes, run through at least one complete scenario:
  - Build user-space utilities successfully
  - Run basic zfs/zpool commands with --help
  - Run file-based sanity tests: `sudo ./scripts/zfs-tests.sh -f -t functional/basic`

### Build Validation Scenarios
Test these scenarios after making changes:
1. **Clean Build Test**: `make clean && ./autogen.sh && ./configure --with-config=user --enable-debug && make -j$(nproc)`
2. **Style Check**: `make checkstyle` 
3. **Basic Functionality**: `sudo ./zfs version && sudo ./zpool version`
4. **Sanity Test**: `sudo ./scripts/zfs-tests.sh -f -t functional/basic` (runs specific basic tests)

## Build Timing Expectations

**NEVER CANCEL builds or tests** - they take significant time but should complete successfully:

- `autogen.sh`: ~12-15 seconds
- `configure`: ~6 seconds (user-space) to ~75 seconds (full)
- `make` (user-space): ~1.5-2 minutes  
- `make` (full with kernel): ~2-5 minutes
- `make checkstyle`: ~30 seconds
- Basic sanity tests: ~25 minutes
- Full test suite: ~1-2+ hours
- Stress tests (zloop): Configurable, typically 10+ minutes

Set timeouts with 50-100% buffer: user-space build timeout should be 10+ minutes, full build 15+ minutes, tests 60+ minutes to 3+ hours.

## Common Tasks

### Repository Structure
The repository is organized as follows:
```
cmd/           - Command-line utilities (zfs, zpool, etc.)
lib/           - User-space libraries (libzfs, libzpool, etc.)  
module/        - Kernel modules and core ZFS implementation
tests/         - Test suites and test utilities
scripts/       - Build and utility scripts
man/           - Manual pages
contrib/       - Contributed utilities and integrations
etc/           - Configuration files and systemd units
```

### Key Files and Directories
- `scripts/zfs-tests.sh` - Main test runner script
- `scripts/zloop.sh` - Stress testing script  
- `autogen.sh` - Autotools bootstrap script
- `configure.ac` - Autotools configuration
- `Makefile.am` - Main makefile template
- `META` - Version and metadata information

### Build Configurations
- `--with-config=user` - Build only user-space utilities (recommended for development)
- `--with-config=kernel` - Build only kernel modules  
- `--with-config=all` - Build both user-space and kernel (default)
- `--enable-debug` - Enable debug builds with assertions and additional checks

### Development Workflow
1. Make changes to source code
2. Run `make clean` if doing significant changes  
3. Build: `./autogen.sh && ./configure --with-config=user --enable-debug && make -j$(nproc)`
4. Test: `make checkstyle`  
5. Validate: Test basic functionality and run sanity tests
6. For kernel changes: Rebuild with full config and test kernel modules

### Troubleshooting
- **Build fails with missing dependencies**: Install the build dependencies listed above
- **Configure fails**: Ensure autotools are installed and run `./autogen.sh` first
- **Kernel module build fails**: Try user-space only build with `--with-config=user`
- **Tests fail to run**: Ensure you have root privileges and required disk space
- **Permission denied errors**: Most ZFS operations require root privileges

### Testing Strategy
- **Unit/Component Tests**: Use specific test tags with `-t` flag
- **Functional Tests**: Run file-based tests with `-f` flag (safer for development)
- **Integration Tests**: Full test suite tests real functionality end-to-end  
- **Stress Tests**: Use `zloop.sh` for stress testing and finding race conditions

Always validate that your changes don't break existing functionality by running the basic test scenario before moving to comprehensive testing.