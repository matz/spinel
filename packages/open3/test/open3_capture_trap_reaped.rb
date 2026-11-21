# A trap that raises after the wait in Open3.capture2e reaped the child
# reaches the caller, and the temporary directory is removed.
require "open3"
require "tmpdir"

dir = Dir.mktmpdir
ENV["TMPDIR"] = dir
trap("CHLD") { raise "chld" }
begin
  Open3.capture2e("true")
  puts "no raise"
rescue => e
  p e.class
  p e.message
end
trap("CHLD", "DEFAULT")
p Dir.children(dir)
Dir.rmdir(dir)
