# spinel: gc-stress
# A writer's open of a FIFO waits for a reader, and the reader here is started
# by another thread that allocates on its way. Under GC stress one of those
# allocations starts a collection, whose barrier waits for every worker to
# park; the thread in the open must park too, else the two wait for each other
# and the reader never comes.
require "tmpdir"
dir = File.join(Dir.tmpdir, "sp_fifo_wait_#{Process.pid}")
Dir.mkdir(dir)
fifo = File.join(dir, "fifo")
File.mkfifo(fifo)
$stdout.flush
t = Thread.new do
  junk = nil
  40.times { junk = Array.new(50) { |k| "junk #{k}" } }
  system("cat", fifo)
  junk.size
end
w = File.open(fifo, "w")
w.puts "through the fifo"
w.close
puts "join #{t.value}"
File.delete(fifo)
Dir.rmdir(dir)
puts "end"
