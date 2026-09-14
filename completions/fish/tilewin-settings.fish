# fish completions for tilewin-settings

complete -c tilewin-settings -f
complete -c tilewin-settings -s p -l page -x -d 'Page to open' -a '
theme\t"Theme and mode"
wallpaper\t"Wallpaper per theme or for all themes"
taskbar\t"Taskbar layout and widgets"
menus\t"Right-click menus and start menu"
launcher\t"Application launcher and default programs"
keyboard\t"Keyboard layouts, keys and shortcuts"'
complete -c tilewin-settings -s h -l help -d 'Show help'
