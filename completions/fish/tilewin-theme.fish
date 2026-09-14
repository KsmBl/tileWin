# fish completions for tilewin-theme

function __tilewin_theme_names
    tilewin-theme list 2>/dev/null | string replace -r '^. (\S+)\s+(.*)$' '$1\t$2'
end

set -l subcommands list current set info path scheme help

complete -c tilewin-theme -f
complete -c tilewin-theme -n "not __fish_seen_subcommand_from $subcommands" -a list -d 'List available themes'
complete -c tilewin-theme -n "not __fish_seen_subcommand_from $subcommands" -a current -d 'Print the active theme'
complete -c tilewin-theme -n "not __fish_seen_subcommand_from $subcommands" -a set -d 'Switch to a theme'
complete -c tilewin-theme -n "not __fish_seen_subcommand_from $subcommands" -a info -d 'Show theme details'
complete -c tilewin-theme -n "not __fish_seen_subcommand_from $subcommands" -a path -d 'Print the theme directory'
complete -c tilewin-theme -n "not __fish_seen_subcommand_from $subcommands" -a help -d 'Show help'
complete -c tilewin-theme -n "__fish_seen_subcommand_from set info path; and test (count (commandline -opc)) -eq 2" -a '(__tilewin_theme_names)'
complete -c tilewin-theme -n "not __fish_seen_subcommand_from $subcommands" -a scheme -d 'Show or change the color scheme (dark mode)'
complete -c tilewin-theme -n "__fish_seen_subcommand_from scheme" -a 'light dark toggle'
