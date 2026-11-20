# Kernel#exec with an environment Hash ahead of the command, as Process.spawn
# takes one: a name with a String value is set for the command, a name with
# nil is unset, and the rest of this process's environment stays. The runtime
# took the command and its arguments only, so the Hash reached it as the
# command and it raised TypeError ("wrong first argument type").
ENV["SP_EXEC_KEEP"] = "kept"
ENV["SP_EXEC_DROP"] = "dropped"
exec({ "SP_EXEC_PROBE" => "yes", "SP_EXEC_DROP" => nil }, "sh", "-c",
     'echo "probe=$SP_EXEC_PROBE keep=$SP_EXEC_KEEP drop=${SP_EXEC_DROP-unset}"')
puts "not reached"
