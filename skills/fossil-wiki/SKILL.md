---
name: fossil-wiki
description: Instructions for AI agents on creating, updating, exporting, and managing Wiki pages and Tech Notes in Fossil SCM.
---

# Fossil Wiki & Tech Notes Skill for AI Agents

This skill provides step-by-step instructions for AI agents to list, view, export, create, and update **Wiki pages** and **Tech Notes** in a **Fossil SCM** repository using the native Fossil CLI.

---

## 1. Quick Overview

Fossil SCM includes a built-in Wiki and Tech Note system stored as versioned artifacts inside the repository. Wiki pages support **Markdown**, **Fossil Wiki**, and **Plain Text** formats.

- **Wiki Pages**: Named documentation pages (e.g. `Home`, `Architecture`, `Setup-Guide`).
- **Tech Notes**: Timestamped events attached to the timeline with optional tags and color-coding, ideal for release notes, design decisions, and log entries.

---

## 2. Command Reference

| Action | Command | Description |
| :--- | :--- | :--- |
| **List Wiki Pages** | `fossil wiki list` | Lists all wiki pages in the repository. |
| **Export Wiki Page** | `fossil wiki export "PageName" -` | Exports page content to stdout (or to a file if filename provided). |
| **Create Wiki Page** | `fossil wiki create "PageName" file.md -M markdown` | Creates a new wiki page from a file or stdin. |
| **Update Wiki Page** | `fossil wiki commit "PageName" file.md -M markdown` | Commits an updated version of an existing wiki page. |
| **List Tech Notes** | `fossil wiki list --technote -s` | Lists technotes with timestamp and unique IDs. |
| **Create Tech Note** | `fossil wiki create "Comment" -t now --technote-tags "tag1,tag2" file.md` | Creates a timestamped tech note on the timeline. |
| **Export Tech Note** | `fossil wiki export -t <TECHNOTE_ID> -` | Displays the content of a specific tech note. |

---

## 3. Supported Formats (Mimetypes)

When creating or updating wiki pages, always specify `-M` (or `--mimetype`):
- `markdown` (or `text/x-markdown`): Recommended for general documentation.
- `fossil` (or `text/x-fossil-wiki`): Fossil's traditional wiki syntax.
- `plain` (or `text/x-plain`): Plain text.

---

## 4. Step-by-Step Guidelines for AI Agents

### A. Listing & Reading Existing Wiki Pages

1. **List all pages**:
   ```bash
   fossil wiki list
   ```

2. **Read/Export a page to stdout**:
   ```bash
   fossil wiki export "Architecture" -
   ```

3. **Export a page to a file**:
   ```bash
   fossil wiki export "Architecture" docs/architecture.md
   ```

---

### B. Creating & Updating Wiki Pages

1. **Creating a New Wiki Page**:
   Create a local file (or write content) and commit it as a new wiki page:
   ```bash
   # Create content in a temporary or scratch file
   echo "# System Architecture\n\nOverview of components..." > /tmp/arch.md
   
   # Register wiki page
   fossil wiki create "Architecture" /tmp/arch.md -M markdown
   ```

2. **Updating an Existing Wiki Page**:
   Use `commit` (instead of `create`) when editing an existing page:
   ```bash
   fossil wiki commit "Architecture" /tmp/arch_v2.md -M markdown
   ```

---

### C. Working with Tech Notes (Timeline Notes)

Tech Notes are timestamped notes ideal for logging architectural decision records (ADRs), release notes, or agent activity logs directly into the Fossil timeline.

1. **Creating a Tech Note**:
   ```bash
   echo "# Release Notes v1.2.0\n\n- Fixed parser bug\n- Updated docs" > /tmp/release.md
   
   fossil wiki create "Release v1.2.0" \
     -t now \
     --technote-tags "release,v1.2.0" \
     --technote-bgcolor "#d0e0f0" \
     -M markdown \
     /tmp/release.md
   ```

2. **Listing & Reading Tech Notes**:
   ```bash
   fossil wiki list --technote -s
   ```
   To export/read a specific tech note by ID:
   ```bash
   fossil wiki export -t <TECHNOTE_ID> -
   ```

---

## 5. SQL Direct Queries for Wiki Data

To query wiki pages via SQL:

```bash
fossil sql --readonly "SELECT substr(name,6) AS pagename FROM tag WHERE name LIKE 'wiki-%';"
```

To list wiki timeline events:

```bash
fossil sql --readonly "SELECT datetime(mtime,'unixepoch'), comment FROM event WHERE type='w' ORDER BY mtime DESC LIMIT 10;"
```

---

## 6. Best Practices for AI Agents

1. **Default to Markdown**: Always pass `-M markdown` when creating or updating wiki pages and tech notes for consistency.
2. **Use Temp Files for Multi-line Content**: Write large wiki pages to a temporary file (`/tmp/page.md`) before running `fossil wiki create` or `fossil wiki commit`.
3. **Clean Up Scratch Files**: Delete temporary draft files after committing them to the wiki.
