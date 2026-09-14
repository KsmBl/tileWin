# fish completions for tilewin and tilewin-session

for cmd in tilewin tilewin-session
    complete -c $cmd -s h -l help -d 'Show help'
    complete -c $cmd -s c -l config -r -F -d 'Use a specific config file'
    complete -c $cmd -s C -l validate -d 'Check the config file and exit'
    complete -c $cmd -s d -l debug -d 'Full debug logging'
    complete -c $cmd -s D -x -a 'noatomic txn-wait txn-timings txn-timeout=' -d 'Debug option'
    complete -c $cmd -s v -l version -d 'Show version'
    complete -c $cmd -s V -l verbose -d 'Verbose logging'
    complete -c $cmd -s m -l mode -x -a 'window tile' -d 'Start in tile or window mode'
    complete -c $cmd -l get-socketpath -d 'Print the IPC socket path'
    complete -c $cmd -l unsupported-gpu -d 'Allow proprietary GPU drivers'
end
