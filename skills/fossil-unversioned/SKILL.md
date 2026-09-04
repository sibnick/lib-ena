---
name: fossil-unversioned
description: Instructions for AI agents on managing unversioned files (build artifacts, downloads, logs, reports) in Fossil SCM using fossil uv.
---

# Fossil Unversioned Files Skill for AI Agents

This skill provides step-by-step instructions for AI agents to store, manage, export, and sync **unversioned files** (`fossil uv`) inside a **Fossil SCM** repository without polluting commit history.

---

## 1. Overview

Fossil SCM includes an **unversioned file store** inside its SQLite repository database. Unversioned files are:
- Kept out of standard version control commit history (no SHA-3 hashes created for commits).
- Automatically synced across repository clones during `fossil sync` or `fossil uv sync`.
- Ideal for **build binaries, test coverage reports, benchmark logs, generated documentation assets, and release packages**.

---

## 2. Command Reference

| Action | Command | Description |
| :--- | :--- | :--- |
| **List Unversioned Files** | `fossil uv list` (or `fossil uv ls -l`) | Shows all unversioned files held in the repository. |
| **Add / Update File** | `fossil uv add <filename>` | Adds or updates an unversioned file in the repo DB. |
| **Add with Custom Name** | `fossil uv add <local_path> --as <uv_path>` | Store a local file under a different path in UV storage. |
| **Cat / Read File** | `fossil uv cat <uv_path>` | Outputs unversioned file content to stdout. |
| **Export to Disk** | `fossil uv export <uv_path> <output_path>` | Exports an unversioned file to local disk. |
| **Remove File** | `fossil uv rm <uv_path>` | Deletes an unversioned file from UV storage. |
| **Sync Remote UV** | `fossil uv sync` | Synchronizes unversioned files with remote repository. |

---

## 3. Step-by-Step Guidelines for AI Agents

### A. Storing Build Artifacts & Test Reports
When an AI agent runs a build or test suite and generates output artifacts (e.g. coverage reports, test logs, compiled binaries):

```bash
# Example: Store test log
fossil uv add build/test-report.html --as reports/latest-test-report.html

# Example: Store compiled binary artifact
fossil uv add dist/app-linux-x64 --as releases/app-linux-x64
```

### B. Inspecting & Retrieving Unversioned Files

1. **List all unversioned files with size and modification timestamp**:
   ```bash
   fossil uv ls -l
   ```

2. **Read content of an unversioned file**:
   ```bash
   fossil uv cat reports/latest-test-report.html
   ```

3. **Export an unversioned file to disk**:
   ```bash
   fossil uv export releases/app-linux-x64 ./dist/app-binary
   ```

---

## 4. Best Practices for AI Agents

1. **Do NOT use UV for source code**: Source code files MUST be tracked in normal check-ins via `fossil add` and `fossil commit`.
2. **Clean up old artifacts**: Use `fossil uv rm <path>` to remove outdated build logs or temporary test run outputs.
3. **Explicit Paths**: Use clear directory prefixes in UV storage (e.g., `reports/`, `releases/`, `assets/`) to keep unversioned files organized.
