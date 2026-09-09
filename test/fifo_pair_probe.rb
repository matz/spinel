# ISOLATION PROBE, not a fix and not meant to stay.
#
# #4394 is that File.open on a FIFO waits inside open(2) with no descriptor to
# wait on, so a green thread pinned to its worker stalls every green thread
# there. The Linux fix (opening a FIFO O_NONBLOCK and clearing the flag) was
# reverted because the test's reader-and-writer arm never completed on macOS.
#
# This file is that arm ALONE, on a tree with no runtime change at all. If it
# hangs here too, passing lines through a FIFO between two green threads was
# already broken on macOS and the reverted change is not what broke it.
dir = "/tmp/sp_fifo_probe_#{Process.pid}"
Dir.mkdir(dir)
pipe = File.join(dir, "pipe")
system("mkfifo #{pipe}") or raise "mkfifo failed"

got = []
rd = Thread.new do
  File.open(pipe, "r") { |f| f.each_line { |l| got << l.chomp } }
end
wr = Thread.new do
  File.open(pipe, "w") { |f| 3.times { |i| f.puts "line#{i}" } }
end
wr.join
rd.join
puts "read #{got.inspect}"
File.delete(pipe) rescue nil
Dir.rmdir(dir) rescue nil
puts "end"
