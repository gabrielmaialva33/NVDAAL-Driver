# Contributing to NVDAAL

First off, thank you for considering contributing to NVDAAL! :tada:

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [How Can I Contribute?](#how-can-i-contribute)
- [Development Setup](#development-setup)
- [Coding Standards](#coding-standards)
- [Pull Request Process](#pull-request-process)

## Code of Conduct

This project adheres to a code of conduct. By participating, you are expected to uphold this code. Please be respectful and constructive in all interactions.

## How Can I Contribute?

### Reporting Bugs

Before creating bug reports, please check existing issues to avoid duplicates.

When creating a bug report, include:
- **GPU Model** (e.g., RTX 4090, RTX 4080)
- **macOS Version** (e.g., Tahoe 26.x)
- **OpenCore Version**
- **Boot Arguments**
- **Driver Logs** (run `make logs`)

### Suggesting Features

Feature requests are welcome! Please provide:
- Clear description of the feature
- Use case / motivation
- Potential implementation approach (if any)

### Code Contributions

1. Check existing issues and PRs
2. For major changes, open an issue first to discuss
3. Fork the repository
4. Create a feature branch
5. Make your changes
6. Submit a PR

## Development Setup

### Prerequisites

```bash
# Xcode Command Line Tools
xcode-select --install

# Clone the repo
git clone https://github.com/gabrielmaialva33/NVDAAL-Driver.git
cd NVDAAL-Driver
```

### Building

```bash
# Clean build
make clean && make

# Validate
make test
```

### Testing

```bash
# Load kext (temporarily)
make load

# Check logs
make logs

# Unload
make unload
```

## Coding Standards

### C++ Style

- Use 4-space indentation (no tabs)
- Opening braces on same line
- Descriptive variable names
- Comment complex logic

```cpp
// Good
bool NVDAALGsp::initQueues(void) {
    if (!allocDmaBuffer(&cmdQueueMem, QUEUE_SIZE, &cmdQueuePhys)) {
        IOLog("NVDAAL-GSP: Failed to allocate command queue\n");
        return false;
    }
    return true;
}

// Bad
bool NVDAALGsp::initQueues(void)
{
if(!allocDmaBuffer(&cmdQueueMem,QUEUE_SIZE,&cmdQueuePhys)){IOLog("fail");return false;}
return true;
}
```

### Naming Conventions

| Type | Convention | Example |
|------|------------|---------|
| Classes | PascalCase with NVDAAL prefix | `NVDAALGsp` |
| Methods | camelCase | `readRegister()` |
| Constants | UPPER_SNAKE_CASE | `NV_PMC_BOOT_0` |
| Variables | camelCase | `chipArch` |
| Macros | UPPER_SNAKE_CASE | `NV_MEMORY_BARRIER()` |

### Log Messages

Always prefix with `NVDAAL` or `NVDAAL-GSP`:

```cpp
IOLog("NVDAAL: Starting driver\n");
IOLog("NVDAAL-GSP: RPC 0x%02x sent\n", function);
```

### Header Guards

Use `#ifndef` style:

```cpp
#ifndef NVDAAL_GSP_H
#define NVDAAL_GSP_H

// ... content ...

#endif // NVDAAL_GSP_H
```

## Pull Request Process

1. **Update Documentation**: If you change functionality, update relevant docs

2. **Test Your Changes**: Ensure `make test` passes

3. **Commit Messages**: Use clear, descriptive commit messages
   ```
   Add GSP firmware loading support

   - Implement ELF parser for gsp.bin
   - Add radix3 page table builder
   - Configure WPR metadata
   ```

4. **PR Description**: Include:
   - What the PR does
   - Why the change is needed
   - How it was tested
   - Any breaking changes

5. **Review**: Wait for maintainer review and address feedback

## Questions?

Feel free to open an issue with the "question" label.

---

Thank you for contributing! :purple_heart:
