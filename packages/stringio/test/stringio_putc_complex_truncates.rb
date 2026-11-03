# StringIO#putc of a Complex converts its real part as to_int does: truncated
# The range check compared the real part before truncating it, so on the
# 32-bit build Complex(-2147483648.5, 0), whose to_int -2147483648 fits a
# long and a C int, raised RangeError.
require "stringio"

def t(label)
  p [label, :ok, yield]
rescue RangeError, IndexError => e
  p [label, e.class, e.message]
end

io = StringIO.new
vals = [1, Complex(-2147483648.5, 0), Complex(66.9, 0), Complex(-190.5, 0)]
t(:edge) { io.putc(vals[ARGV.size + 1]).class }
t(:frac) { io.putc(vals[ARGV.size + 2]).class }
t(:neg) { io.putc(vals[ARGV.size + 3]).class }
p io.string.bytes
t(:fetch_edge) { [1, 2].fetch(vals[ARGV.size + 1]) }
