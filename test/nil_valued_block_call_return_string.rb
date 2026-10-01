# A method whose value is a block `return` of a String or the nil a block call
# answers when it runs out: File.foreach's block form is `(lines.each {}; nil)`
# underneath, and that nil came back through the String slot as the int 0.
require "tmpdir"

dir = Dir.mktmpdir
path = File.join(dir, "lines.txt")
File.write(path, "one\ntwo\nthree;four\n")
short = File.join(dir, "short.txt")
File.write(short, "a\nb\n")
File.write(File.join(dir, "zz_long_name"), "")

def first_long(path) = File.foreach(path) { |l| return l if l.size > 4 }
p first_long(path)
p first_long(short)

def first_long_def(path)
  File.foreach(path) { |l| return l if l.size > 4 }
end
p first_long_def(path)
p first_long_def(short)

def io_first_long(path) = IO.foreach(path) { |l| return l.chomp if l.size > 4 }
p io_first_long(path)
p io_first_long(short)

def long_child(dir) = Dir.each_child(dir) { |e| return e if e.size > 10 }
p long_child(dir)
def long_entry(dir) = Dir.foreach(dir) { |e| return e if e.size > 10 }
p long_entry(dir)

def first_wide(a) = (a.each { |s| return s if s.size > 1 }; nil)
p first_wide(["a", "bb"])
p first_wide(["a"])

def first_long_line(s) = s.each_line { |l| return l if l.size > 4 }
p first_long_line("ab\ncdefgh\n")

def line_size(path) = File.foreach(path) { |l| return l.size if l.size > 4 }
p line_size(path)
p line_size(short)

def line_array(path) = File.foreach(path) { |l| return [l] if l.size > 4 }
p line_array(path)
p line_array(short)
