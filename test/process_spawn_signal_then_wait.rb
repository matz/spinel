# A SIGTERM that arrives while Process.spawn waits for its child to exec is
# raised when Process.wait begins, before the child exits. The spawned child
# reads a byte from the pipe named by SPINEL_TEST_SPAWN_HOLD_FD before it
# execs; the helper starts on a line from inside the begin, sends SIGTERM
# 0.3 s later and writes the byte at 0.6 s. CRuby ignores the variable, and
# the SIGTERM arrives in Process.wait. After the rescue, a safe point (the
# recursive call) finds no signal recorded.
r, w = IO.pipe
go_r, go_w = IO.pipe
helper = Process.spawn("sh", "-c", "read go; sleep 0.3; kill -TERM $PPID; sleep 0.3; echo x", in: go_r, out: w)
go_r.close
ENV["SPINEL_TEST_SPAWN_HOLD_FD"] = r.fileno.to_s
t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
pid = nil
begin
  go_w.puts "go"
  pid = Process.spawn("sleep", "5")
  Process.wait(pid)
  puts "no signal"
rescue SignalException => e
  puts "rescued #{e.message}"
end
ENV.delete("SPINEL_TEST_SPAWN_HOLD_FD")
p Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0 < 2
p pid.is_a?(Integer)
def depth(k, acc)
  return acc if k == 0
  acc << k
  depth(k - 1, acc)
end
p depth(ARGV.size + 3, []).size
Process.kill(:KILL, pid) if pid
Process.wait(pid) if pid
Process.wait(helper)
