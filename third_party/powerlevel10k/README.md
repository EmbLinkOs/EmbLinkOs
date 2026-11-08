# Powerlevel10k (reference)

From Powerlevel10k 1.20.0 (https://github.com/romkatv/powerlevel10k), MIT
licence (LICENSE). These are DATA for EmbLink's prompt engine, not code it
runs -- there is no zsh here:

- `config/p10k-{lean,lean-8colors,classic,rainbow,pure}.zsh`: the style
  templates `p10k configure` starts from. `prompt style lean` reads the lean
  one through the same importer as a person's own `~/.p10k.zsh`
  (shell/prompt/import.c), so a style means exactly what it means there.
- `internal/icons.zsh`: the icon table per POWERLEVEL9K_MODE;
  tools/p10k_icons.py turns it into shell/prompt/icons.c.

Unmodified copies; update them together, and regenerate icons.c.
