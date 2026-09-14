# fish completions for tilewinmsg (tileWin IPC client)

function __tilewinmsg_themes
    tilewin-theme list 2>/dev/null | string replace -r '^. (\S+)\s+(.*)$' '$1\t$2'
end

function __tilewinmsg_workspaces
    tilewinmsg -t get_workspaces -r 2>/dev/null | string match -r '"name": "[^"]*"' | string replace -r '"name": "(.*)"' '$1'
end

function __tilewinmsg_command_is
    set -l tokens (commandline -opc)
    set -l skip 0
    set -l words
    for token in $tokens[2..-1]
        if test $skip -eq 1
            set skip 0
            continue
        end
        switch $token
            case -t --type -s --socket
                set skip 1
            case '-*'
            case '*'
                set -a words $token
        end
    end
    if test (count $argv) -eq 0
        test (count $words) -eq 0
        return
    end
    test (count $words) -ge 1; and contains -- $words[1] $argv; and test (count $words) -eq 1
end

function __tilewinmsg_no_command
    __tilewinmsg_command_is
end

set -l types command get_workspaces get_inputs get_outputs get_tree get_marks \
    get_bar_config get_version get_binding_modes get_binding_state get_config \
    get_seats send_tick subscribe get_tilewin

complete -c tilewinmsg -f
complete -c tilewinmsg -s h -l help -d 'Show help'
complete -c tilewinmsg -s m -l monitor -d 'Monitor until killed (with -t subscribe)'
complete -c tilewinmsg -s p -l pretty -d 'Pretty output'
complete -c tilewinmsg -s q -l quiet -d 'Be quiet'
complete -c tilewinmsg -s r -l raw -d 'Raw JSON output'
complete -c tilewinmsg -s s -l socket -r -F -d 'IPC socket'
complete -c tilewinmsg -s v -l version -d 'Show version'
complete -c tilewinmsg -s t -l type -x -a "$types" -d 'Message type'

# tileWin commands
complete -c tilewinmsg -n __tilewinmsg_no_command -a mode -d 'Switch tile/window mode'
complete -c tilewinmsg -n __tilewinmsg_no_command -a wm_mode -d 'Switch tile/window mode'
complete -c tilewinmsg -n __tilewinmsg_no_command -a theme -d 'Switch theme'
complete -c tilewinmsg -n __tilewinmsg_no_command -a arrange -d 'Arrange windows on the workspace'
complete -c tilewinmsg -n __tilewinmsg_no_command -a snap -d 'Snap the focused window'
complete -c tilewinmsg -n __tilewinmsg_no_command -a maximize -d 'Maximize the focused window'
complete -c tilewinmsg -n __tilewinmsg_no_command -a minimize -d 'Minimize the focused window'
complete -c tilewinmsg -n __tilewinmsg_no_command -a showdesktop -d 'Show the desktop'
complete -c tilewinmsg -n __tilewinmsg_no_command -a alttab -d 'Window switcher'
complete -c tilewinmsg -n __tilewinmsg_no_command -a restart -d 'Restart tileWin or the taskbar'
complete -c tilewinmsg -n __tilewinmsg_no_command -a panel -d 'Taskbar actions'
complete -c tilewinmsg -n __tilewinmsg_no_command -a launcher -d 'Open the application launcher'
complete -c tilewinmsg -n __tilewinmsg_no_command -a wallpaper -d 'Set the wallpaper'
complete -c tilewinmsg -n __tilewinmsg_no_command -a panel_command -d 'Taskbar program'
complete -c tilewinmsg -n __tilewinmsg_no_command -a launcher_command -d 'Launcher program or builtin'
complete -c tilewinmsg -n __tilewinmsg_no_command -a session_restore -d 'Reopen apps at login'
# common sway commands
complete -c tilewinmsg -n __tilewinmsg_no_command -a reload -d 'Reload the config'
complete -c tilewinmsg -n __tilewinmsg_no_command -a exit -d 'Exit tileWin'
complete -c tilewinmsg -n __tilewinmsg_no_command -a exec -d 'Run a program'
complete -c tilewinmsg -n __tilewinmsg_no_command -a workspace -d 'Switch workspace'
complete -c tilewinmsg -n __tilewinmsg_no_command -a focus -d 'Move focus'
complete -c tilewinmsg -n __tilewinmsg_no_command -a move -d 'Move window or workspace'
complete -c tilewinmsg -n __tilewinmsg_no_command -a kill -d 'Close the focused window'
complete -c tilewinmsg -n __tilewinmsg_no_command -a floating -d 'Toggle floating'
complete -c tilewinmsg -n __tilewinmsg_no_command -a fullscreen -d 'Toggle fullscreen'
complete -c tilewinmsg -n __tilewinmsg_no_command -a layout -d 'Change the container layout'
complete -c tilewinmsg -n __tilewinmsg_no_command -a split -d 'Split the container'
complete -c tilewinmsg -n __tilewinmsg_no_command -a resize -d 'Resize the container'
complete -c tilewinmsg -n __tilewinmsg_no_command -a scratchpad -d 'Show the scratchpad'
complete -c tilewinmsg -n __tilewinmsg_no_command -a sticky -d 'Toggle sticky'
complete -c tilewinmsg -n __tilewinmsg_no_command -a border -d 'Change the border style'
complete -c tilewinmsg -n __tilewinmsg_no_command -a output -d 'Configure an output'
complete -c tilewinmsg -n __tilewinmsg_no_command -a input -d 'Configure an input device'
complete -c tilewinmsg -n __tilewinmsg_no_command -a seat -d 'Configure a seat'
complete -c tilewinmsg -n __tilewinmsg_no_command -a nop -d 'Do nothing'

# arguments of tileWin commands
complete -c tilewinmsg -n '__tilewinmsg_command_is mode wm_mode' -a 'tile window toggle'
complete -c tilewinmsg -n '__tilewinmsg_command_is theme' -a '(__tilewinmsg_themes)'
complete -c tilewinmsg -n '__tilewinmsg_command_is arrange' -a 'cascade horizontal vertical optimal'
complete -c tilewinmsg -n '__tilewinmsg_command_is snap' -a 'left right up down topleft topright bottomleft bottomright restore'
complete -c tilewinmsg -n '__tilewinmsg_command_is maximize minimize floating fullscreen sticky' -a 'enable disable toggle'
complete -c tilewinmsg -n '__tilewinmsg_command_is alttab' -a 'next prev commit cancel'
complete -c tilewinmsg -n '__tilewinmsg_command_is restart' -a 'panel\t"Restart only the taskbar" relaunch-apps\t"Restart and reopen apps"'
complete -c tilewinmsg -n '__tilewinmsg_command_is panel' -a 'startmenu run launcher calendar network volume power activate window_menu menu reload close'
complete -c tilewinmsg -n '__tilewinmsg_command_is wallpaper' -a 'theme none solid gradient image'
complete -c tilewinmsg -n '__tilewinmsg_command_is workspace' -a 'next prev next_on_output prev_on_output back_and_forth number (__tilewinmsg_workspaces)'
complete -c tilewinmsg -n '__tilewinmsg_command_is focus' -a 'left right up down parent child mode_toggle floating tiling output'
complete -c tilewinmsg -n '__tilewinmsg_command_is layout' -a 'default splith splitv stacking tabbed toggle'
complete -c tilewinmsg -n '__tilewinmsg_command_is exec' -a '(__fish_complete_command)'
complete -c tilewinmsg -n '__tilewinmsg_command_is launcher_command' -a 'builtin'
complete -c tilewinmsg -n '__tilewinmsg_command_is session_restore' -a 'yes no'
