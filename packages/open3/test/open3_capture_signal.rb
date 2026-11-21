# A signal that interrupts Open3.capture2e waits for the child before the
# SignalException reaches the caller, and the child is reaped. The child
# sleeps before it signals: a signal that lands while the parent is still in
# Process.spawn raises there, before capture2e holds the child's pid, and
# nothing waits (on a loaded machine the child ran that far ahead).
require "open3"
require "tmpdir"

dir = Dir.mktmpdir
marker = File.join(dir, "done")
begin
  Open3.capture2e("sh", "-c", "sleep 0.2; kill -TERM $PPID; sleep 0.3; echo done > \"$1\"", "sh", marker)
  puts "no signal"
rescue SignalException => e
  p e.message
  p File.exist?(marker)
end
begin
  Process.wait
rescue Errno::ECHILD
  puts "reaped"
end
File.delete(marker) if File.exist?(marker)
Dir.rmdir(dir)
