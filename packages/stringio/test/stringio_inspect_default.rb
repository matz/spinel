# A StringIO inspects as Object#inspect's default, #<StringIO:0x...>
# A native class with no #inspect of its own had no arm in the object
# inspect dispatcher and no row in its ivar walk, which then answered
# "#<Object>": directly, inside an Array or a Hash, and boxed. Its #to_s
# already rendered #<StringIO:0x...>, and so did a subclass's #inspect.
require "stringio"

io = StringIO.new(+"ab")
pat = /\A#<StringIO:0x[0-9a-f]+>\z/
p io.inspect.match?(pat), io.to_s.match?(pat)
p io.inspect == io.to_s
p [io].inspect.match?(/\A\[#<StringIO:0x[0-9a-f]+>\]\z/)
p({k: io}.inspect.match?(/\A\{k: #<StringIO:0x[0-9a-f]+>\}\z/))
v = [io, 1][ARGV.size]
p v.inspect.match?(pat), v.to_s.match?(pat)
p "#{io}".match?(pat)
p (io << "z").inspect.match?(pat)

class Buf < StringIO; end
b = Buf.new
p b.inspect.match?(/\A#<Buf:0x[0-9a-f]+>\z/), [b].inspect.include?("#<Buf:0x")
