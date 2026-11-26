# File#each_char, #each_byte, #each_codepoint, #each_line and #each without a
# block return an Enumerator that reads the file as it is walked. A forwarder
# with `&block` calls them that way when it is called without a block (the
# tempfile package's each_char and each_byte).
require "tmpdir"
path = File.join(Dir.tmpdir, "spinel_io_each_char_no_block_#{Process.pid}.txt")
File.write(path, "ab\nc")

f = File.open(path)
p f.each_char.to_a
f.rewind
p f.each_byte.to_a
f.rewind
p f.each_codepoint.to_a
f.rewind
p f.each_line.to_a
f.rewind
p f.each.to_a
f.rewind
e = f.each_char
p [e.next, e.next]
p e.class
f.rewind
e = f.each_char
pairs = []
e.each { |c| pairs << [c, f.getc] }
p pairs
f.rewind
e = f.each_char
p [e.next, f.getc, e.next]
f.close

class Wrap
  def initialize(path) = @f = File.open(path)
  def each_char(&block) = @f.each_char(&block)
  def each_byte(&block) = @f.each_byte(&block)
  def close = @f.close
end
w = Wrap.new(path)
p w.each_char.to_a
w.close
w = Wrap.new(path)
w.each_byte { |b| print b, " " }
puts
w.close

require "tempfile"
t = Tempfile.new("x")
t.write("xyz")
t.rewind
p t.each_char.to_a
t.rewind
p t.each_byte.map { |b| b + 1 }
t.close
File.delete(path)
