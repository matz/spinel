# A flagged wait in a green thread: the thread polls and hands the scheduler
# back, so the main thread runs on while the child is waited for, with
# WUNTRACED (a stopped child ends the wait) and with WNOHANG (nil at once).
pid = Process.spawn("sleep", "5")
sleep 0.2
t = Thread.new { Process.wait2(pid, Process::WUNTRACED) }
ticks = 0
while t.alive? && ticks < 50
  ticks += 1
  sleep 0.02
  Process.kill("STOP", pid) if ticks == 3
end
r, st = t.value
p r == pid
p st.stopped?
p ticks >= 3 && ticks < 50

t2 = Thread.new { Process.wait(pid, Process::WNOHANG) }
p t2.value

Process.kill("CONT", pid)
Process.kill("KILL", pid)
_, st = Process.wait2(pid)
p st.signaled?
