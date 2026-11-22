# sleep and Process.wait end on time while a trapped signal arrives in a
# loop. On Linux the remainder nanosleep writes after EINTR includes the
# timer slack, and a retry with that remainder did not end.
trap("USR1") { }
killer = Process.spawn("sh", "-c", "while kill -USR1 $PPID 2>/dev/null; do :; done")
puts "start"
t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
sleep 0.2
p Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0 < 5
t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
pid = Process.spawn("sleep", "0.2")
Process.wait(pid)
p Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0 < 5
Process.kill(:KILL, killer)
Process.wait(killer)
