savedcmd_button_driver.mod := printf '%s\n'   button_driver.o | awk '!x[$$0]++ { print("./"$$0) }' > button_driver.mod
