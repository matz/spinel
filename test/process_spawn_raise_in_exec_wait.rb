# A Thread#raise sent while Process.spawn waits for its child to exec is
# raised after the spawn returns the pid, so the child is reaped. The child
# reads a byte from the pipe named by SPINEL_TEST_SPAWN_HOLD_FD before it
# execs; the raise is sent once the main thread sleeps in that wait, and the
# byte is written after it. CRuby ignores the variable, and the raise lands
# in the sleep after the spawn.
r, w = IO.pipe
ENV["SPINEL_TEST_SPAWN_HOLD_FD"] = r.fileno.to_s
main = Thread.current
t = Thread.new do
  Thread.pass until main.status == "sleep"
  main.raise(RuntimeError, "from thread")
  w.write("x")
end
pid = nil
begin
  pid = Process.spawn("true")
  sleep 5
rescue RuntimeError => e
  puts "rescued #{e.message}"
end
ENV.delete("SPINEL_TEST_SPAWN_HOLD_FD")
t.join
p pid.is_a?(Integer)
Process.wait(pid) if pid
begin
  Process.wait
  puts "child left"
rescue Errno::ECHILD
  puts "reaped"
end
