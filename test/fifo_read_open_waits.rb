# A read-only open of a FIFO waits for a writer, as CRuby's does: a process
# handed the descriptor reads what the writer sends, where an open that
# returned before any writer gave it the end of the file at once. The
# reading is a child's (cat), since the test is about the open; the second
# round has another thread alive, which takes the open down its other path.
require "tmpdir"
fifo = File.join(Dir.tmpdir, "sp_fifo_read_open_#{Process.pid}")
system("mkfifo", fifo)
def round(fifo)
  writer = spawn("sh", "-c", "sleep 0.5; echo data > #{fifo}")
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  File.open(fifo, "r") do |f|
    p (Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0) > 0.4
    system("cat", in: f)
  end
  Process.wait(writer)
end
round(fifo)
puts "one thread done"
t = Thread.new { sleep 3 }
round(fifo)
t.kill
t.join
puts "two threads done"
# a writer already there: the open answers at once and the child reads
writer = spawn("sh", "-c", "echo early > #{fifo}")
sleep 0.3
File.open(fifo, "r") { |f| system("cat", in: f) }
Process.wait(writer)
File.delete(fifo)
puts "end"
