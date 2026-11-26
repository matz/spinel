# each_line, each_byte, each_char, eof, getc, readchar, readbyte, lineno,
# ungetbyte, ungetc and the mode predicates on an IO read back out of an
# Array or a boxed instance variable, beside the same calls on a typed IO
# and on a boxed String, and respond_to? for each name.
def pipe_with(text)
  r, w = IO.pipe
  w.write(text)
  w.close
  r
end

def boxed(text) = [pipe_with(text), 1][0]

a = []
boxed("ab\ncd\n").each_line { |l| a << l }
p a
p boxed("ab\ncd\n").each_line.to_a
p boxed("x;y;z").each_line(";").to_a
b = []
boxed("ab").each_byte { |x| b << x }
p b
ch = []
boxed("hé").each_char { |x| ch << x }
p ch
p boxed("ab").each_byte.to_a
p boxed("ab").each_char.to_a
r = boxed("q")
p r.eof
p r.getc
p r.getc
p r.eof
p boxed("xy").readchar
p boxed("xy").readbyte
r = boxed("a\nb\n")
r.gets
p r.lineno
r = boxed("ab")
r.ungetbyte(0x41)
p r.read(2)
r = boxed("ab")
r.ungetc("Z")
p r.read(2)
r = boxed("ab")
r.ungetbyte("XY")
p r.read(4)

# a bounded walk of an open pipe reads only what it takes
open_r, open_w = IO.pipe
open_w.write("abcdef")
p [open_r, 1][0].each_byte.first(2)
p [open_r, 1][0].each_char.first(2)
open_w.close

require "tmpdir"
path = File.join(Dir.tmpdir, "spinel_boxed_lines_#{Process.pid}.txt")
File.write(path, "l1\nl2\n")
f = [File.open(path), 1][0]
e = f.each_line
p e.next
e.rewind
p e.next
f.close
g = [File.open(path), 1][0]
e = g.each_line("2", chomp: true)
p e.next
e.rewind
p e.next
g.close
File.delete(path)
p boxed("").autoclose?, boxed("").binmode?, boxed("").close_on_exec?
p boxed("x").to_io.class

class Holder
  def initialize(io) = @io = io
  def io = @io
end
h = Holder.new(pipe_with("k\n"))
Holder.new(1)
e = []
h.io.each_line { |l| e << l }
p e

t = pipe_with("ab\n")
p t.getc, t.readbyte, t.lineno, t.eof

s = ["ab\n", 1][0]
sl = []
s.each_line { |l| sl << l }
sb = []
s.each_byte { |x| sb << x }
sc = []
s.each_char { |x| sc << x }
p sl, sb, sc, s.each_byte.to_a

names = %i[to_io sysread syswrite each_line each_byte each_char eof getc lineno readbyte readchar ungetbyte ungetc autoclose? binmode? close_on_exec?]
p names.reject { |m| boxed("").respond_to?(m) }
p names.reject { |m| pipe_with("").respond_to?(m) }
