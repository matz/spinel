# A builtin class the program reopens keeps the counts of its own class
# methods for every name the program does not define, keywords counted as
# CRuby counts them; a name the program defines keeps the program's.
require "tmpdir"
src = File.join(Dir.tmpdir, "spinel_io_class_counts_reopened.txt")
File.write(src, "abc\n")
f = File.open(src)
fd = f.fileno
def t; yield; rescue => e; p [e.class, e.message]; end
class IO
  def self.other = 1
  def self.select(*) = :mine
end
t { IO.for_fd(fd, "r", 3) }
t { IO.for_fd(fd, "r", 0, autoclose: false) }
t { p IO.for_fd(fd, autoclose: false).fileno == fd }
t { IO.sysopen(src, 0, 0644, 1) }
t { p IO.other }
t { IO.other(1) }
t { p IO.select(1, 2, 3, 4, 5) }
t { IO.name(1) }
f.close
File.delete(src)
