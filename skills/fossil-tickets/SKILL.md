---
name: fossil-tickets
description: Instructions for AI agents on managing bug reports, tasks, and issue tracking using Fossil SCM's built-in ticket system and CLI.
---

# Fossil Ticket and Task Tracking Skill for AI Agents

This skill gives step-by-step instructions for AI agents to create, view, search, and update tickets in a **Fossil SCM** repository.

---

## 1. Quick Overview

Fossil SCM includes an issue tracking system inside the SQLite repository database. Tickets synchronize automatically across clones along with code check-ins.

---

## 2. Core Command Reference

| Action | Command | Description |
| :--- | :--- | :--- |
| **Discover Fields** | `fossil ticket ls fields` | Lists all defined ticket database fields. |
| **List Reports** | `fossil ticket ls reports` | Lists predefined ticket report views. |
| **Create Ticket** | `fossil ticket add title "..." type "..." status "Open" comment "..."` | Creates a new ticket. |
| **View Ticket Details** | `fossil ticket show 0 "tkt_uuid LIKE 'UUID%'"` | Shows all fields for matching tickets (Report 0). |
| **View Ticket History** | `fossil ticket history <UUID>` | Shows the audit trail and comments for a ticket. |
| **Update Ticket** | `fossil ticket change <UUID> status "Closed" resolution "Fixed"` | Updates fields on an existing ticket. |
| **Append Comment** | `fossil ticket change <UUID> +comment "Note text"` | Appends text to the ticket comment field. |
| **Query via SQL** | `fossil sql --readonly "SELECT ... FROM ticket;"` | Runs custom SQL queries against the ticket database. |

---

## 3. Common Ticket CLI Pitfalls

### Pitfall 1: `fossil ticket show <UUID>` fails
- **Wrong**: `fossil ticket show 7dfc4d0a44`
  - *Result*: `unknown report format(7dfc4d0a44)!`
- **Why**: The first argument to `fossil ticket show` must be a report number (`0`, `1`, etc.).
- **Correct Methods**:
  1. Use report `0` with a SQL WHERE clause:
     ```bash
     fossil ticket show 0 "tkt_uuid LIKE '7dfc4d0a44%'"
     ```
  2. Use `fossil ticket history`:
     ```bash
     fossil ticket history 7dfc4d0a44
     ```
  3. Use read-only SQL:
     ```bash
     fossil sql --readonly "SELECT * FROM ticket WHERE tkt_uuid LIKE '7dfc4d0a44%';"
     ```

---

## 4. SQLite Schema Reference for `ticket` Table

When querying Fossil tickets with `fossil sql --readonly`, use these exact columns:

| Column Name | Data Type | Description |
| :--- | :--- | :--- |
| `tkt_id` | `INTEGER PRIMARY KEY` | Internal integer ID. |
| `tkt_uuid` | `TEXT` | 40-character unique hex identifier. |
| `tkt_ctime` | `REAL` | Creation Julian day timestamp. |
| `tkt_mtime` | `REAL` | Modification Julian day timestamp. |
| `type` | `TEXT` | `Code_Defect`, `Build_Error`, `Documentation`, `Feature_Request`, `Incident`, `Tweak`. |
| `status` | `TEXT` | `Open`, `Verified`, `In_Review`, `Fixed`, `Closed`, `Deferred`, `Rejected`. |
| `priority` | `TEXT` | `High`, `Medium`, `Low`. |
| `severity` | `TEXT` | `Critical`, `Severe`, `Important`, `Minor`, `Cosmetic`. |
| `subsystem` | `TEXT` | Component name (e.g. `driver`, `pci`, `admin`, `netdev`). |
| `title` | `TEXT` | Single-line summary title of the issue or task. |
| `comment` | `TEXT` | Full description, reproduction steps, or conversation log. |
| `resolution` | `TEXT` | `Fixed`, `Wont_Fix`, `Duplicate`, `Unable_to_Reproduce`. |
| `foundin` | `TEXT` | Branch or version where the bug was found. |

> [!WARNING]
> Do NOT query non-existent columns like `num` or `id`. Use `tkt_uuid` or `tkt_id`.

---

## 5. Copy-Paste SQL Query Templates

### A. List All Open Tickets
```bash
fossil sql --readonly "SELECT substr(tkt_uuid,1,10) AS id, type, priority, status, title FROM ticket WHERE status NOT IN ('Closed', 'Fixed');"
```

### B. View Single Ticket Details
```bash
fossil sql --readonly "SELECT substr(tkt_uuid,1,10) AS id, type, priority, status, resolution, title, comment FROM ticket WHERE tkt_uuid LIKE '7dfc4d0a44%';"
```

### C. Search Tickets by Keyword
```bash
fossil sql --readonly "SELECT substr(tkt_uuid,1,10) AS id, status, priority, title FROM ticket WHERE title LIKE '%Phase 3%';"
```

---

## 6. Step-by-Step Ticket Workflows

### A. Creating a New Ticket
```bash
fossil ticket add \
  title "Implement Phase 4 circular ring buffers" \
  type "Feature_Request" \
  priority "High" \
  severity "Important" \
  status "Open" \
  subsystem "driver" \
  comment "Implement DMA memory allocation and TX/RX ring buffers."
```

### B. Appending Progress Notes to a Ticket
Use `+comment` to add notes without deleting existing text:
```bash
fossil ticket change <10-char-UUID> +comment "

Started work on feature/phase4-dma branch. Created mock descriptor rings."
```

### C. Closing and Resolving a Ticket
```bash
fossil ticket change <10-char-UUID> \
  status "Closed" \
  resolution "Fixed" \
  +comment "

Completed in check-in [<commit-hash>] on feature/phase4-dma. All unit tests pass."
```
