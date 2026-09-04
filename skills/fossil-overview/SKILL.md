---
name: fossil-overview
description: Master entry-point and hub skill providing an overview of Fossil SCM architecture, core CLI concepts, troubleshooting guide, and specialized Fossil skills.
---

# Fossil SCM Master Hub and Feature Overview for AI Agents

This is the primary entry point for AI coding agents in repositories managed by **Fossil SCM**. It provides an architectural overview, a fast troubleshooting guide, and links to specialized Fossil skills.

---

## 1. What is Fossil SCM?

Fossil is a distributed software configuration management system. Unlike Git, Fossil integrates:
1. **Version Control System (VCS)**
2. **Built-in Bug and Issue Tracker**
3. **Built-in Wiki and Tech Notes System**
4. **Unversioned Artifact Storage (`fossil uv`)**
5. **Native Git and GitHub Mirroring (`fossil git`)**
6. **Built-in Web UI (`fossil ui`)**

All project data is stored in a single SQLite database file (`.fossil`).

---

## 2. Specialized Fossil Skills Index

Consult the relevant specialized skill document for specific tasks:

| Task / Domain | Skill File | When to Consult |
| :--- | :--- | :--- |
| **Core SCM and Code** | `skills/fossil-scm/SKILL.md` | Checking status, tracking files, committing, branching (`trunk`), merging, diffing revisions, or inspecting check-ins. |
| **Bug Tracking and Issues** | `skills/fossil-tickets/SKILL.md` | Creating tickets, updating status, querying open issues via SQL, reading ticket history. |
| **Documentation and Wiki** | `skills/fossil-wiki/SKILL.md` | Writing wiki pages, exporting docs, creating timeline Tech Notes (release notes, ADRs). |
| **Build Assets and Reports**| `skills/fossil-unversioned/SKILL.md` | Storing test logs, reports, benchmarks, or binaries in Fossil UV storage (`fossil uv`). |
| **Git / GitHub Sync** | `skills/fossil-git-sync/SKILL.md` | Setting up and maintaining an automated mirror to GitHub (`fossil git export --autopush`). |

---

## 3. Fast Troubleshooting: Common Agent Errors and Fixes

| Symptom / Error | Root Cause | Solution |
| :--- | :--- | :--- |
| `unknown report format(xyz)!` | Ran `fossil ticket show <UUID>` directly without report ID. | Run `fossil ticket show 0 "tkt_uuid LIKE 'xyz%'"` or `fossil ticket history xyz`. |
| `not found: '<hash>'` during `fossil diff` | Positional arguments in Fossil diff are interpreted as filenames. | Use `fossil diff --from <hash1> --to <hash2>`. |
| `fossil: unknown command: ls-tree` | `git ls-tree` does not exist in Fossil. | Use `fossil ls -r <commit-hash>`. |
| `Error: in prepare, no such column: num` | Hallucinated column name in SQL query against `ticket`. | Use `tkt_uuid` or `tkt_id`. |
| `Error: in prepare, no such table: manifest` | Hallucinated Git-like internal table names in Fossil SQLite. | Use `fossil info <hash>` or `fossil ls -r <hash>` instead of direct schema queries. |
| `fossil changes` shows nothing for commits | `fossil changes` only checks local workspace against checkout baseline. | Use `fossil info <hash>` to inspect commit contents. |
| Stray session files in `fossil extras` | Session or log files not in ignore list. | Update `.fossil-settings/ignore-glob`. |

---

## 4. Quick Triage Commands

When first opening a project workspace, run these commands:

```bash
# 1. Verify Fossil CLI installation
fossil version

# 2. Check checkout status and active branch
fossil status
fossil branch current

# 3. View repository metadata and leaf check-in
fossil info

# 4. View recent commit timeline
fossil timeline -n 10

# 5. List open tickets
fossil sql --readonly "SELECT substr(tkt_uuid,1,10) AS id, type, priority, status, title FROM ticket WHERE status NOT IN ('Closed', 'Fixed');"
```

---

## 5. Golden Rules for AI Agents

1. **No Staging Area**: `fossil commit` commits all modified tracked files. You do not need `git add` for modified files.
2. **Never Delete Checkout Files**: Never delete `.fslckout` or `_FOSSIL_` in the project root.
3. **Use `trunk` as Default Branch**: Fossil uses `trunk` instead of `main` or `master`.
4. **Use `fossil update` to Switch Branches**: Switch branches using `fossil update <branch>`.
5. **Safety Net**: If a merge or commit produces unintended results, run `fossil undo`.
