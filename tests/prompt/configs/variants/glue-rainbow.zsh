# On top of p10k-rainbow.zsh: joined segments, a prefix and a suffix, the icon's place and colour, a subsegment separator.
typeset -g POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(dir vcs_joined newline prompt_char)
typeset -g POWERLEVEL9K_RIGHT_PROMPT_ELEMENTS=(status command_execution_time_joined background_jobs time_joined)
typeset -g POWERLEVEL9K_STATUS_OK=true
typeset -g POWERLEVEL9K_TIME_PREFIX='at '
typeset -g POWERLEVEL9K_TIME_ICON_BEFORE_CONTENT=true
typeset -g POWERLEVEL9K_DIR_SUFFIX='%F{196}!'
typeset -g POWERLEVEL9K_DIR_VISUAL_IDENTIFIER_COLOR=226
typeset -g POWERLEVEL9K_DIR_ICON_BEFORE_CONTENT=false
typeset -g POWERLEVEL9K_RIGHT_SUBSEGMENT_SEPARATOR='%F{196}|'
