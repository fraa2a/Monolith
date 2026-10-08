# Repository instructions

Follow `CLAUDE.md` and `docs/DEVELOPMENT_RULES.md` for build, architecture and verification rules.

## Text and comments

- Never use U+2013 or U+2014 in any context: code, comments, documentation, commit messages, pull requests or reports. Use an ASCII hyphen, comma, colon or parentheses as appropriate.
- Remove or replace these characters whenever encountered. Keep all tracked UTF-8 text free of them.
- Keep comments short and essential. Explain only non-obvious intent, constraints or logic. Do not claim that code works, is safe or is correct in a comment.
- Keep names short, clear and specific to the function or type.

## Contributions

- Work on separate branches and open pull requests in English.
- Report exact checks performed and distinguish static inspection from Windows runtime verification.
- Update `docs/handover/ACTIVE_HANDOVER.md` after substantial work.
