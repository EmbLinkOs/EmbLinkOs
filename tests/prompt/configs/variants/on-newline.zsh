# On top of p10k-lean.zsh: one line of elements, put on its own line by PROMPT_ON_NEWLINE.
typeset -g POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(dir vcs)
typeset -g POWERLEVEL9K_RIGHT_PROMPT_ELEMENTS=(status command_execution_time time)
typeset -g POWERLEVEL9K_PROMPT_ON_NEWLINE=true
typeset -g POWERLEVEL9K_MULTILINE_FIRST_PROMPT_PREFIX='%240F╭─'
typeset -g POWERLEVEL9K_MULTILINE_LAST_PROMPT_PREFIX='%240F╰─ '
