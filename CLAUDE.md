# gnash — project conventions

## Commit messages

Follow Conventional Commits 1.0.0 (https://www.conventionalcommits.org/en/v1.0.0/).

- Format: `<type>(<scope>): <description>` — scope is the component, e.g.
  `feat(executor)`, `fix(nameref)`, `perf(glob)`, `docs(release)`.
- Types: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `ci`,
  `chore`, `style`. Breaking changes: append `!` after the type/scope or add a
  `BREAKING CHANGE:` footer.
- Historically this repo used a bare `component: description` prefix
  (`executor:`, `nameref:`); new commits keep the component as the scope and
  lead with the type.
- Releases: `chore(release): gnash X.Y.Z`; release notes: `docs(release): add
  gnash X.Y.Z release notes`.
- Never add a `Co-Authored-By` trailer.
