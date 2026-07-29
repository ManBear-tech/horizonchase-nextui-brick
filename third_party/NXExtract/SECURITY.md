# Security policy

NXExtract handles untrusted ZIP-family containers and writes into a game
directory, so parser and path-safety reports are treated as security issues.

Use the repository’s
[private security advisory form](https://github.com/NextOs-Ports/NXExtract/security/advisories/new).
Do not publish a working traversal, symlink or overwrite exploit before a fix
is available.

Useful reports include:

- the smallest synthetic archive or recipe that reproduces the issue;
- the exact NXExtract version;
- expected and actual destination paths;
- whether live data, staging data or only logs were affected.

Never attach copyrighted game data, credentials, private addresses or personal
logs. A synthetic ZIP with dummy bytes is sufficient.
