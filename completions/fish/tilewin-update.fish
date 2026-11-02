complete -c tilewin-update -f
complete -c tilewin-update -n __fish_use_subcommand -a release -d 'The newest release from GitHub'
complete -c tilewin-update -n __fish_use_subcommand -a source -d 'Build the newest source in a temporary folder'
complete -c tilewin-update -n __fish_use_subcommand -a check -d 'What is installed and the newest release'
complete -c tilewin-update -l force -d 'Also when the release is what is installed'
complete -c tilewin-update -l no-restart -d 'Leave out tilewinmsg restart'
