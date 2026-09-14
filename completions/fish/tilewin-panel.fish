# fish completions for tilewin-panel

complete -c tilewin-panel -s h -l help -d 'Show help'
complete -c tilewin-panel -s v -l version -d 'Show version'
complete -c tilewin-panel -s c -l config -r -F -d 'Taskbar config file'
complete -c tilewin-panel -s s -l socket -r -F -d 'tileWin IPC socket'
complete -c tilewin-panel -s d -l debug -d 'Verbose logging'
