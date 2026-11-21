# A second signal that interrupts the wait for the child in
# Open3.capture2e still removes the temporary directory.
require "open3"
require "tmpdir"

dir = Dir.mktmpdir
ENV["TMPDIR"] = dir
n = 0
trap("USR1") { n += 1; raise "usr1 #{n}" }
begin
  Open3.capture2e("sh", "-c", "kill -USR1 $PPID; sleep 0.2; kill -USR1 $PPID; sleep 0.3", "sh")
  puts "no raise"
rescue => e
  p e.message
end
trap("USR1", "DEFAULT")
p Dir.children(dir)
Dir.rmdir(dir)
