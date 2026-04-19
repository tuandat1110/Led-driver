savedcmd_bbb_glue.mod := printf '%s\n'   bbb_glue.o | awk '!x[$$0]++ { print("./"$$0) }' > bbb_glue.mod
