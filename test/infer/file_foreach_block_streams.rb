# File.foreach's block form streams: no readlines in the emitted C (infer-test).
require "tmpdir"
path = File.join(Dir.tmpdir, "sp_infer_file_foreach_block_streams_#{Process.pid}.txt")
File.write(path, "a\nbb\n")
n = 0
File.foreach(path) { |l| n += l.size }
File.foreach(path, chomp: true) { |l| n += l.size }
File.foreach(path, "b", 1) { |l| n += l.size }
sep = "b"
File.foreach(path, sep, chomp: true) { |l| n += l.size }
p n
File.delete(path)
