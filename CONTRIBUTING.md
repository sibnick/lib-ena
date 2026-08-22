# Contributing to Unikraft ENA Driver

This document explains how to contribute to the ENA driver repository.

## Developer Certificate of Origin (DCO)

All contributions must follow the Developer Certificate of Origin version 1.1.

Add a Signed-off-by line to every commit message:

```
Signed-off-by: Random J Developer <random@developer.example.org>
```

Use your real name. Do not use fake names or anonymous IDs.

## Commit Message Format

Follow the Unikraft commit message conventions:

- Provide a short summary line in the format `<subsystem>/<component>: <summary>` or `<type>(<component>): <summary>`.
- Use the imperative mood (for example, "add feature" not "added feature").
- Limit the summary line to 72 characters.
- Add an empty line after the summary line.
- Provide descriptive details in the commit message body.
- Include ticket references in brackets (for example, `[Ticket <UUID>]`).
- Include the `Signed-off-by` line at the end of the commit message.

## Pull Request Workflow

1. Create a branch from `trunk` for your changes.
2. Implement your code changes and add tests.
3. Make sure all unit tests pass before you commit.
4. Run `make clean && make test` to verify your changes.
5. Commit your changes with a DCO sign-off line.
6. Open a pull request against the `trunk` branch.

## Coding Conventions

Follow the coding conventions defined in [docs/conventions.md](docs/conventions.md).
Write code in standard C99.
Compile code without warnings using `-Wall -Wextra -Werror -pedantic`.
