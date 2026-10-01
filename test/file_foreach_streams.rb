# File.foreach with a block reads its file a line at a time -- File.open and
# the IO's each_line -- rather than reading every line into an array first, and
# closes it however the block leaves (break, return, raise). The separator,
# limit and chomp: arguments mean what they mean to each_line; another keyword
# (mode:) keeps the readlines form, and so does the blockless Enumerator.
require "tmpdir"
path = File.join(Dir.tmpdir, "sp_file_foreach_streams_#{Process.pid}.txt")
File.write(path, "one\ntwo\n\nthree;four\nfive")
a = []; p File.foreach(path) { |l| a << l }; p a
b = []; File.foreach(path, chomp: true) { |l| b << l }; p b
c = []; File.foreach(path, ";") { |l| c << l }; p c
d = []; File.foreach(path, 4) { |l| d << l }; p d
e = []; File.foreach(path, "o", 2, chomp: true) { |l| e << l }; p e
g = []; File.foreach(path, "") { |l| g << l }; p g
n = 0; File.foreach(path) { |l| n += 1; break if n == 2 }; p n
def first_long(path)
  File.foreach(path) { |l| return l if l.size > 4 }
  nil
end
p first_long(path)
k = 0; File.foreach(path) { |l| next if l == "\n"; k += 1 }; p k
begin
  File.foreach(path) { |l| raise ArgumentError, "x" if l.start_with?("two") }
rescue ArgumentError => ex
  p ex.message
end
p File.foreach(path).to_a.size
z = []; File.foreach(path, mode: "r") { |l| z << l }; p z.size
File.delete(path)
