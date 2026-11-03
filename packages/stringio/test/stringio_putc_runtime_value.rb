# StringIO#putc of a value only the run time knows
# A nil, a boxed value or a Float went to the [:int] binding, converted at
# the call before putc ran: a stream that takes no write raised the
# conversion's TypeError where CRuby raises IOError first, and a boxed
# String raised TypeError where CRuby writes its first character. Such an
# argument now reaches putc as a value (the [:any] binding), which checks
# the stream, then writes a String or converts the rest as an Integer.
require "stringio"

def t(label)
  p [label, :ok, yield]
rescue TypeError, IOError, RangeError => e
  p [label, e.class, e.message]
end

puts "-- a stream that takes no write"
ro = StringIO.new("abc".freeze)
v = [1, "x", nil][ARGV.size + 2]
t(:literal_nil) { ro.putc(nil) }
t(:boxed_nil) { ro.putc(v) }
t(:boxed_sym) { ro.putc([:a, 1][ARGV.size]) }
t(:float) { ro.putc(1.5) }

puts "-- a writable stream"
vals = [nil, :a, true, 2**70, Float::NAN, 1e30, -1, 321, 65.9, Object.new, "xy", 66]
io = StringIO.new
vals.each_with_index { |_, i| t(i) { io.putc(vals[i + ARGV.size]).class } }
t(:literal_nil) { io.putc(nil) }
t(:float) { io.putc(67.7) }
p io.string.bytes

puts "-- #to_int, Rational and Complex convert; putc answers its argument"
class N
  def to_int = 67
end
io3 = StringIO.new
conv = [1, N.new, Rational(68, 1), Complex(69, 0)]
t(:to_int) { io3.putc(conv[ARGV.size + 1]).class }
t(:rational) { io3.putc(conv[ARGV.size + 2]).class }
t(:complex) { io3.putc(conv[ARGV.size + 3]).class }
t(:typed_to_int) { io3.putc(N.new).class }
p io3.string

puts "-- typed arguments as before"
io2 = StringIO.new
io2.putc(66)
io2.putc("AZ")
p io2.string
