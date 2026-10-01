# File.foreach and an IO's each_line read a line whole: one longer than 64KB
# is not cut into pieces, and a NUL byte neither ends the line nor drops the
# rest of it.
require "tmpdir"
path = File.join(Dir.tmpdir, "sp_file_foreach_long_nul_lines_#{Process.pid}.txt")
File.write(path, "x" * 100000 + "\nshort\n" + "y" * 70000)
r = []; File.foreach(path) { |l| r << l.size }; p r
r = []; File.foreach(path, chomp: true) { |l| r << l.size }; p r
r = []; File.open(path) { |f| f.each_line { |l| r << l.size } }; p r
File.binwrite(path, "a\0b\nc\0\n\0")
r = []; File.foreach(path) { |l| r << l }; p r
r = []; File.foreach(path, chomp: true) { |l| r << l }; p r
r = []; File.open(path) { |f| f.each_line { |l| r << l } }; p r
File.delete(path)
