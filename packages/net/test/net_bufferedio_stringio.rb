require "net/protocol"
require "stringio"

def show(label)
  puts label
  yield
rescue EOFError => error
  puts error.class.name
end

lines = Net::BufferedIO.new(StringIO.new("head\r\nnext\n"))
show("readline") { p lines.readline, lines.readline }
show("readline EOF") { p lines.readline }

parts = Net::BufferedIO.new(StringIO.new("one|two|tail"))
show("readuntil") { p parts.readuntil("|"), parts.readuntil("|") }
show("readuntil EOF") { p parts.readuntil("|") }
show("readuntil ignore EOF") { p parts.readuntil("|", true), parts.readuntil("|", true) }

reader = Net::BufferedIO.new(StringIO.new("abcdef"))
dest = +"prefix:"
show("read") { p reader.read(2), reader.read(3, dest).equal?(dest), dest }
show("read short") { p reader.read(2, dest) }
p dest
show("read ignore EOF") { p reader.read(1, +"", true), reader.read(0) }

show("read_all") do
  all = Net::BufferedIO.new(StringIO.new("a\0b\r\nc"))
  output = +"prefix:"
  p all.read_all(output).equal?(output), output.bytes, all.read_all
end

show("multiple fills") do
  large = Net::BufferedIO.new(StringIO.new("x" * 16_384 + "end\n"))
  p large.read(16_383).bytesize, large.readline, large.read_all
end

empty = Net::BufferedIO.new(StringIO.new(""))
show("empty readline") { p empty.readline }
show("empty readuntil") { p empty.readuntil("|") }
show("empty readuntil ignore EOF") { p empty.readuntil("|", true) }
show("empty read") { p empty.read(1) }
show("empty read ignore EOF") { p empty.read(1, +"", true), empty.read(0) }
show("empty read_all") { p empty.read_all, empty.read_all }
