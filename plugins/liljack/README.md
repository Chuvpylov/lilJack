# liljack plugin

Packaging of the lilJack hooks + MCP server + skill as a Claude Code plugin.
The scripts live in `toolbox/`; this plugin only wires them.

Install from the local marketplace (this repo's `plugins/` dir):

```bash
claude plugin marketplace add /path/to/liljack/plugins
claude plugin install liljack@brain
```

⚠ The same three hooks are wired in `~/.claude/settings.json` (the pre-plugin
form). Enabling the plugin while those stay is a double fire — remove the
`hooks` block from settings.json when switching. `LILJACK_TOOLBOX_ROOT_ROOT` overrides
the toolbox location.
