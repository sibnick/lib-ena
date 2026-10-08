# OpenCode Agent Instructions & Repository Guidelines

This document provides system directives, technical standards, and workflow instructions for OpenCode agents working in this repository.

---

## 1. Core Operating Principles

1. **Fossil SCM is Primary**: This repository uses **Fossil SCM** (not Git) as its primary configuration management, issue tracker, wiki, and artifact store.
2. **Simplified Technical English (STE)**: All prose (documentation, commit messages, ticket updates, release notes, code comments) must follow ASD-STE100 principles.
3. **Safety & Non-Destruction**:
   - Never delete or modify `.fslckout` or `_FOSSIL_` database files.
   - Do not attempt history rewriting (no rebase/reset; use `fossil amend` or `fossil undo`).
   - Run SQL queries with `--readonly` flag: `fossil sql --readonly "..."`.

---

## 2. Writing Style: ASD-STE100 (Simplified Technical English)

Apply these rules to all written prose (not code or command syntax) to ensure clear, precise, and slop-free communication:

### Style Rules
- **Active Voice**: Write "the parser processes the input", not "the input is processed by the parser".
- **Short Common Words**:
  - `start` (not begin / commence / initiate)
  - `use` (not utilize / leverage)
  - `help` (not facilitate)
  - `make sure` (not ensure)
  - `before` (not prior to)
  - `after` (not subsequent to)
  - `about` (not regarding / concerning)
  - `get` (not obtain / acquire)
  - `show` (not demonstrate)
  - `also` (not additionally / furthermore / moreover)
- **No Marketing Fluff**: Remove adjectives like *seamless, robust, powerful, cutting-edge, effortless, next-generation*.
- **Sentence Limits**: Max 20 words per instruction sentence; max 25 words per descriptive sentence.
- **Punctuation**: Do not use semicolons. Split into separate sentences.
- **One Term Per Concept**: Keep naming consistent throughout. Do not use multiple terms for the same entity.

---

## 3. Fossil SCM Workflows

### A. Environment Verification
When starting a session:
```bash
fossil version
fossil status
fossil branch current
fossil timeline -n 10
```

### B. Daily Development Cycle
- **Status & Changes**:
  ```bash
  fossil status       # Shows checkout status & modified files
  fossil changes      # Lists modified tracked files
  fossil extras       # Lists untracked files not in ignore list
  fossil diff         # Inspects uncommitted workspace changes
  ```
- **Diffing & Inspecting Revisions**:
  ```bash
  fossil diff --from <rev1> --to <rev2>  # Diff between two check-ins (use --from/--to, not positional args)
  fossil diff --checkin <commit-hash>    # Diff introduced by a specific commit
  fossil info <commit-hash>              # Commit metadata and list of changed files
  fossil ls -r <commit-hash>             # List files at revision (replaces git ls-tree)
  fossil finfo -l <file>                 # File history timeline
  ```
- **Tracking & Removing Files**:
  ```bash
  fossil add <file>       # Registers newly created file for tracking (do not use for modified files)
  fossil addremove        # Automatically tracks new files & flags deleted ones
  fossil forget <file>    # Untracks file without deleting local copy
  fossil rm <file>        # Deletes file and stages removal
  ```
- **Commits**:
  - **No Staging Index**: `fossil commit` automatically commits all modified tracked files.
  - Commits automatically push to remote if autosync is enabled.
  ```bash
  fossil commit -m "feat(module): add network packet processing"
  # Commit specific files only:
  fossil commit -m "docs: update API reference" docs/api.md
  ```

### C. Branching & Merging
- Default branch is **`trunk`** (not `main` or `master`).
- **Create feature branch**:
  ```bash
  fossil branch new feature-network trunk
  fossil update feature-network
  ```
  *Or commit directly to a new branch:*
  ```bash
  fossil commit --branch feature-network -m "Start network driver implementation"
  ```
- **Switch branch**:
  ```bash
  fossil update <branch_name>
  ```
- **Merge branch**:
  ```bash
  fossil update trunk
  fossil merge feature-network
  fossil commit -m "Merge feature-network into trunk [Ticket <UUID>]"
  ```
- **Undo mistakes**:
  ```bash
  fossil undo         # Reverts last commit, update, merge, or revert
  ```

---

## 4. Fossil Bug & Task Tracking (Tickets)

Fossil integrates bug tracking inside the repository database.

### Core Commands
- **List open tickets**:
  ```bash
  fossil sql --readonly "SELECT substr(tkt_uuid,1,10) AS id, type, priority, status, title FROM ticket WHERE status NOT IN ('Closed', 'Fixed');"
  ```
- **Create a ticket**:
  ```bash
  fossil ticket add \
    title "Short summary of issue" \
    type "Code_Defect" \
    priority "High" \
    severity "Severe" \
    status "Open" \
    subsystem "core" \
    comment "Reproduction steps and technical details."
  ```
- **View ticket details & history**:
  ```bash
  fossil ticket show 0 "tkt_uuid LIKE '<UUID_PREFIX>%'"  # Report 0 dump with SQL filter
  fossil ticket history <UUID_PREFIX>                    # Audit history & comments
  ```
- **Append progress comment**:
  ```bash
  fossil ticket change <UUID_PREFIX> +comment "\n\nInvestigated root cause in parser. Branch created."
  ```
- **Close ticket**:
  ```bash
  fossil ticket change <UUID_PREFIX> \
    status "Closed" \
    resolution "Fixed" \
    +comment "\n\nResolved in check-in [<COMMIT_HASH>]."
  ```
- **Commit reference**: Reference ticket UUIDs in commit messages using brackets: `[Ticket <10-char-UUID>]`.

> [!CAUTION]
> Do not run `fossil ticket show <UUID>` without a report number. Always specify report `0` or use `fossil ticket history`.

---

## 5. End-to-End Task Lifecycle for Subagents

Subagents working on feature milestones or bug fixes must complete the full cycle:

1. **Triage**: Check current state: `fossil status && fossil branch current && fossil timeline -n 5`.
2. **Branch**: `fossil branch new feature/<name> trunk && fossil update feature/<name>`.
3. **Develop & Test**: Implement code and run test harness (`make clean && make test`).
4. **Track & Review**: Run `fossil add <new_files>` or `fossil addremove`, then inspect with `fossil status` and `fossil diff`.
5. **Commit**: `fossil commit -m "feat(<subsystem>): <summary>\n\n<details>\n[Ticket <UUID>]"`.
6. **Close Ticket**: `fossil ticket change <UUID> status "Closed" resolution "Fixed" +comment "..."`.
7. **Merge to Trunk**: `fossil update trunk && fossil merge feature/<name> && fossil commit -m "Merge feature/<name> into trunk [Ticket <UUID>]"`.
8. **Verify Trunk**: Run `make clean && make test` and verify `fossil status` is clean.

---

## 6. Fossil Wiki & Tech Notes

Use Fossil's built-in wiki and tech notes for documentation and timeline records.

- **List & export wiki pages**:
  ```bash
  fossil wiki list
  fossil wiki export "Architecture" docs/architecture.md
  ```
- **Create or update wiki page**:
  ```bash
  fossil wiki create "PageName" file.md -M markdown
  fossil wiki commit "PageName" file.md -M markdown
  ```
- **Create Tech Note (ADR / Release Note on timeline)**:
  ```bash
  fossil wiki create "Release v1.0.0" \
    -t now \
    --technote-tags "release,v1.0.0" \
    --technote-bgcolor "#d0e0f0" \
    -M markdown \
    /tmp/release-notes.md
  ```

---

## 7. Unversioned File Store (`fossil uv`)

Use unversioned storage for build binaries, test reports, logs, and benchmark results without bloating commit history:

- **Add/Update artifact**:
  ```bash
  fossil uv add build/test-report.html --as reports/test-report.html
  fossil uv add dist/binary --as releases/app-linux-x64
  ```
- **List / Cat / Export**:
  ```bash
  fossil uv ls -l
  fossil uv cat reports/test-report.html
  fossil uv export releases/app-linux-x64 ./dist/app-binary
  ```
- **Sync unversioned data**:
  ```bash
  fossil uv sync
  ```

---

## 8. Git & GitHub Synchronization (`fossil git`)

When mirroring the primary Fossil repository to GitHub:

- **Recommended: use this script**:
  ```bash
  scripts/git_export.sh
  ```
  It runs `fossil sync`, then `fossil git export`, then repairs the mirror state
  (see below), and prints `fossil git status`.
- **Incremental sync to mirror**:
  ```bash
  fossil git export ../git-mirror --mainbranch main --autopush https://github.com/org/repo.git
  ```
- **Subsequent exports**:
  ```bash
  fossil git export
  ```
- **Mirror status**:
  ```bash
  fossil git status
  ```

> [!NOTE]
> `fossil git export` fills the mirror with `git fast-import` and pushes with
> `git push --mirror <url>`. It does not update the mirror index or
> `refs/remotes/origin/*`. After a bare export, run these two commands so
> `git status` and `git branch -vv` report correctly:
> ```bash
> git reset -q      # Match the index to the exported tip, and keep the working tree.
> git fetch -q origin
> ```

---

## 9. Skills Directory Reference

Detailed reference documents are available in the `skills/` directory:

| Skill | Path | Description |
| :--- | :--- | :--- |
| **Fossil Overview** | `skills/fossil-overview/SKILL.md` | Master entry point, troubleshooting table, triage commands. |
| **Fossil SCM** | `skills/fossil-scm/SKILL.md` | Core SCM, checkout, addremove, commits, branch/merge rules, gotchas. |
| **Fossil Tickets** | `skills/fossil-tickets/SKILL.md` | Ticket creation, schema reference, SQL templates, lifecycle management. |
| **Fossil Wiki** | `skills/fossil-wiki/SKILL.md` | Wiki pages, markdown formatting, timeline Tech Notes. |
| **Fossil Unversioned** | `skills/fossil-unversioned/SKILL.md` | Storing build logs, test outputs, and binaries in `fossil uv`. |
| **Fossil Git Sync** | `skills/fossil-git-sync/SKILL.md` | Mirroring Fossil repository to Git / GitHub. |
| **STE Writing** | `skills/ste-writing/SKILL.md` | ASD-STE100 writing guidelines and slop-removal linter. |
| **EC2 Perf Testing** | `skills/ec2-perf-testing/SKILL.md` | Deploying, measuring, and reporting EC2 HTTP performance tests. |

