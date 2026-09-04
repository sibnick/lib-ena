---
name: fossil-git-sync
description: Instructions for AI agents on setting up and managing Git and GitHub mirrors for Fossil SCM repositories using fossil git.
---

# Fossil Git & GitHub Integration Skill for AI Agents

This skill provides instructions for AI agents to set up, manage, and execute incremental synchronization between a primary **Fossil SCM** repository and a **Git / GitHub mirror**.

---

## 1. Overview

Fossil SCM features built-in, native Git interoperability (`fossil git`). It allows project maintainers to keep Fossil as their primary development repository while mirroring all check-ins to a local Git directory or a remote GitHub repository (for GitHub Actions, public visibility, or CI/CD pipelines).

---

## 2. Command Reference

| Action | Command | Description |
| :--- | :--- | :--- |
| **Export to Git Mirror** | `fossil git export <MIRROR_DIR>` | Incrementally exports Fossil check-ins to a local Git repo at `<MIRROR_DIR>`. |
| **Export with Auto-Push** | `fossil git export <MIRROR_DIR> --autopush <GIT_REMOTE_URL>` | Exports check-ins to Git mirror and automatically runs `git push` to GitHub. |
| **Check Mirror Status** | `fossil git status` | Displays status of active Git mirror. |
| **Specify Main Branch** | `fossil git export <MIRROR_DIR> --mainbranch main` | Maps Fossil's `trunk` branch to `main` (or `master`) in Git. |

---

## 3. Step-by-Step Guidelines for AI Agents

### A. Setting Up a GitHub Mirror for a Fossil Repo

1. **Create Git Mirror Directory**:
   ```bash
   mkdir -p ../git-mirror
   ```

2. **Initialize Incremental Export with Auto-Push**:
   ```bash
   fossil git export ../git-mirror \
     --mainbranch main \
     --autopush https://github.com/user/project-mirror.git
   ```

3. **Subsequent Synchronization**:
   After committing new check-ins in Fossil, run:
   ```bash
   fossil git export
   ```
   Fossil automatically remembers the mirror directory and `--autopush` target URL and updates GitHub incrementally.

### B. Branch Name Translation
- Fossil branch `trunk` $\rightarrow$ Git branch `main` (if `--mainbranch main` is specified) or `master`.
- All feature branches and tags in Fossil are automatically converted into corresponding Git branches and tags during export.

---

## 4. Best Practices for AI Agents

1. **Single Source of Truth**: Always perform core development, ticket edits, and commits inside Fossil. Treat the Git repository as a read-only mirror.
2. **Do Not Touch `.mirror_state`**: Fossil stores internal export state in `<MIRROR_DIR>/.mirror_state`. Never modify or delete files in this directory.
3. **Automate After Fossil Commit**: If working on a project with an active Git mirror, run `fossil git export` after major Fossil commits to keep GitHub in sync.
