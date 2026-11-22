# A Thread#raise sent while Process.spawn runs leaves no child unreaped,
# and a later trap runs. The spawn opens the FIFO for the child stdin, and
# the FIFO's writer opens it at 1 s. CRuby raises the RuntimeError sent at
# 0.3 s in that open, before the fork. Spinel raises it after the spawn
# returns the pid.
require "tmpdir"

dir = Dir.mktmpdir
fifo = File.join(dir, "fifo")
system("mkfifo", fifo)
system("sh", "-c", "(sleep 1; : <> \"$1\") >/dev/null 2>&1 &", "sh", fifo)
main = Thread.current
t = Thread.new do
  sleep 0.3
  main.raise(RuntimeError, "from thread")
end
pid = nil
begin
  pid = Process.spawn("true", in: fifo)
  sleep 5
rescue RuntimeError => e
  puts "rescued #{e.message}"
end
t.join
Process.wait(pid) if pid
begin
  Process.wait
  puts "child left"
rescue Errno::ECHILD
  puts "reaped"
end
ran = false
trap("USR1") { ran = true }
Process.kill(:USR1, Process.pid)
sleep 0.3
p ran
File.delete(fifo)
Dir.rmdir(dir)
