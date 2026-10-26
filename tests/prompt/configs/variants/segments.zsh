# On top of p10k-lean.zsh: the context, and status, duration, jobs and time as they can also be.
typeset -g POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(context dir vcs newline prompt_char)
typeset -g POWERLEVEL9K_RIGHT_PROMPT_ELEMENTS=(status command_execution_time background_jobs time)
typeset -g POWERLEVEL9K_CONTEXT_TEMPLATE='ctx'   # literal: %n is the recording Mac's own user
typeset -g POWERLEVEL9K_CONTEXT_DEFAULT_CONTENT_EXPANSION='${P9K_CONTENT}'
unset POWERLEVEL9K_CONTEXT_DEFAULT_VISUAL_IDENTIFIER_EXPANSION
typeset -g POWERLEVEL9K_STATUS_OK=true
typeset -g POWERLEVEL9K_STATUS_ERROR=true
typeset -g POWERLEVEL9K_STATUS_VERBOSE_SIGNAME=true
typeset -g POWERLEVEL9K_COMMAND_EXECUTION_TIME_PRECISION=1
typeset -g POWERLEVEL9K_BACKGROUND_JOBS_VERBOSE_ALWAYS=true
typeset -g POWERLEVEL9K_TIME_FORMAT='%D{%H:%M}'
