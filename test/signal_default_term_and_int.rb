# A SIGTERM and a SIGINT that arrive while Process.spawn waits for its child
# to exec are both raised: one when Process.wait begins and the other in the
# sleep after the rescue. The spawned child reads a byte from the pipe named
# by SPINEL_TEST_SPAWN_HOLD_FD before it execs; the helper starts on a line
# from inside the begin, sends SIGTERM at 0.3 s, SIGINT at 0.4 s and writes
# the byte at 0.7 s. CRuby ignores the variable, and the signals arrive in
# Process.wait and the sleep. The order differs, so the classes are sorted.
r, w = IO.pipe
go_r, go_w = IO.pipe
helper = Process.spawn("sh", "-c", "read go; sleep 0.3; kill -TERM $PPID; sleep 0.1; kill -INT $PPID; sleep 0.3; echo x", in: go_r, out: w)
go_r.close
ENV["SPINEL_TEST_SPAWN_HOLD_FD"] = r.fileno.to_s
caught = []
pid = nil
begin
  begin
    go_w.puts "go"
    pid = Process.spawn("sleep", "5")
    Process.wait(pid)
  rescue SignalException => e
    caught << e.class.name
  end
  sleep 2
rescue SignalException => e
  caught << e.class.name
end
ENV.delete("SPINEL_TEST_SPAWN_HOLD_FD")
p caught.sort
Process.kill(:KILL, pid) if pid
Process.wait(pid) if pid
Process.wait(helper)
