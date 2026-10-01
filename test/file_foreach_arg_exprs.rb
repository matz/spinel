# A streaming File.foreach whose arguments after the path run code: they are
# evaluated before the file is opened, in order, and an Integer one is still
# the limit (the readlines form read it as the separator).
require "tmpdir"
path = File.join(Dir.tmpdir, "sp_file_foreach_arg_exprs_#{Process.pid}.txt")
File.write(path, "one\ntwo\r\nthree;four\n")
log = []
def sep(log) = (log << :sep; ";")
def ch(log) = (log << :ch; true)
def lim = 3
def pick(i) = i == 0 ? ";" : nil

File.foreach(path, sep(log), chomp: ch(log)) { |l| log << l }
p log
log = []
begin
  File.foreach(path + ".missing", sep(log), chomp: ch(log)) { |l| log << l }
rescue Errno::ENOENT
  log << :enoent
end
p log
log = []
begin
  File.foreach((log << :path; path + ".missing"), (log << :lim; 2)) { }
rescue Errno::ENOENT
  log << :enoent
end
p log

x = 2
File.foreach(path, x + 0) { |l| p l; break }
File.foreach(path, lim) { |l| p l; break }
File.foreach(path, "\n", lim) { |l| p l; break }
File.foreach(path, pick(0), chomp: true) { |l| p l }
File.foreach(path, pick(1)) { |l| p l }
[1, 2].each do |i|
  File.foreach(path, "\n", lim + i) { |l| p [i, l]; break }
end

# a later argument assigns an earlier local: each is read in order
s = "\n"
File.foreach(path, s, (s = ";"; 50)) { |l| p l }
p s
q = path
File.foreach(q, (q = "nope"; "\n")) { |l| p l }
p q
File.delete(path)
