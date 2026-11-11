# Process.wait, waitpid, wait2 and waitpid2 take a flags argument, and with
# Process::WNOHANG answer nil while the child runs on, then the pid (and
# the status) once it has ended. $? follows the child a flagged wait reaps,
# as it does after a plain wait.
pid = Process.spawn("sleep", "0.3")
p Process.wait2(pid, Process::WNOHANG)
p Process.waitpid(pid, Process::WNOHANG)
p Process.wait(pid, Process::WNOHANG)
p Process.waitpid2(pid, Process::WNOHANG)

# flags of 0 wait as the plain form does
r, st = Process.wait2(pid, 0)
p r == pid
p st.class
p st.exitstatus
p $?.pid == pid

# a child that has already ended is reaped at once, and $? is its status
pid = Process.spawn("/usr/bin/false")
sleep 0.2
p Process.wait(pid, Process::WNOHANG) == pid
p $?.exitstatus
pid = Process.spawn("/usr/bin/true")
sleep 0.2
r, st = Process.waitpid2(pid, Process::WNOHANG)
p r == pid
p st.success?

# the constants are the platform's Integers
p Process::WNOHANG.is_a?(Integer)
p Process::WUNTRACED.is_a?(Integer)
p Process::WNOHANG != Process::WUNTRACED
