# Kernel#exec with the options of Process.spawn: the redirections of the
# standard streams (an IO for in: and err:), chdir: and a leading environment
# Hash together. The compiler refused the Hash ("exec with an options Hash").
# The process becomes the command, with the options applied to itself, so
# the command's output is this program's.
r, w = IO.pipe
w.write "from-pipe\n"
w.close
exec({ "SP_EXEC_PROBE" => "yes" }, "sh", "-c",
     'echo "in=$(cat)"; echo "pwd=$(pwd)"; echo "to-err probe=$SP_EXEC_PROBE" >&2',
     in: r, err: $stdout, chdir: "/")
puts "not reached"
