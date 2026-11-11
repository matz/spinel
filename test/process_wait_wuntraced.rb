# Process.wait2 with Process::WUNTRACED answers a stopped child too, with a
# Process::Status that says so: stopped? is true and stopsig the signal,
# while exited?, signaled?, exitstatus and termsig answer as for a child
# that has not ended. A plain wait after the child is continued and killed
# answers the end, where stopped? is false and stopsig nil.
pid = Process.spawn("sleep", "5")
sleep 0.2
Process.kill("STOP", pid)
r, st = Process.wait2(pid, Process::WUNTRACED)
p r == pid
p st.stopped?
p st.stopsig == Signal.list["STOP"]
p st.exited?
p st.signaled?
p st.exitstatus
p st.termsig
p st.success?
p $?.stopped?

Process.kill("CONT", pid)
Process.kill("TERM", pid)
r, st = Process.wait2(pid)
p r == pid
p st.stopped?
p st.stopsig
p st.signaled?
p st.termsig == Signal.list["TERM"]
