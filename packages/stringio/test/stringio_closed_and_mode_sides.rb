# A StringIO reads and writes only on the sides it is open for
# The stream kept no record of its mode or of #close: a closed one, or one
# opened "r", took every write, and a closed one or one opened "w" or "a"
# read on. CRuby raises IOError ("not opened for writing" / "... reading")
# before it looks at the arguments. #closed? is true only once both sides
# are closed, and #string= keeps the sides as they were.
require "stringio"

def t(label)
  r = yield
  p [label, :ok, r]
rescue IOError => e
  p [label, e.class, e.message]
end

def closed_io = StringIO.new(+"ab\ncd").tap(&:close)

puts "-- closed: writers"
t(:putc_s) { closed_io.putc("x") }
t(:putc_i) { closed_io.putc(66) }
t(:putc_empty) { closed_io.putc("") }
t(:write) { closed_io.write("y") }
t(:write_empty) { closed_io.write("") }
t(:shl) { (closed_io << "z").class }
t(:puts) { closed_io.puts("w") }
t(:puts_bare) { closed_io.puts }
t(:print) { closed_io.print("v") }
t(:print_nil) { closed_io.print(nil) }
t(:print_none) { closed_io.print }
t(:print_nils) { closed_io.print(nil, nil) }
t(:puts_empty_array) { closed_io.puts([]) }   # writes nothing, and CRuby does not ask
t(:write_none) { closed_io.write }
t(:write_nil) { closed_io.write(nil) }
t(:write_nils) { closed_io.write(nil, nil) }
t(:r_write_nil) { StringIO.new("ab", "r").write(nil) }
t(:truncate) { closed_io.truncate(0) }

puts "-- closed: readers"
t(:read) { closed_io.read }
t(:read_n) { closed_io.read(1) }
t(:read_neg) { closed_io.read(-1) }
t(:gets) { closed_io.gets }
t(:gets_sep) { closed_io.gets("\n") }
t(:gets_nil) { closed_io.gets(nil) }
t(:gets_limit) { closed_io.gets(1) }
t(:gets_chomp) { closed_io.gets(chomp: true) }
t(:getc) { closed_io.getc }
t(:getbyte) { closed_io.getbyte }
t(:readbyte) { closed_io.readbyte }
t(:readchar) { closed_io.readchar }
t(:readline) { closed_io.readline }
t(:readline_sep) { closed_io.readline("\n") }
t(:readlines) { closed_io.readlines }
t(:readlines_sep) { closed_io.readlines("\n") }
t(:eof) { closed_io.eof? }

puts "-- closed: the rest still answers"
t(:closed_p) { closed_io.closed? }
t(:pos) { closed_io.pos }
t(:size) { closed_io.size }
t(:string) { closed_io.string }
t(:rewind) { closed_io.rewind }
t(:close_again) { closed_io.close }

puts "-- modes"
r = StringIO.new(+"ab", "r")
t(:r_read) { r.read }
t(:r_write) { r.write("x") }
t(:r_putc) { r.putc(65) }
t(:r_putc_empty) { r.putc("") }
t(:frozen_putc_empty) { StringIO.new("ab".freeze).putc("") }
t(:putc_empty_ok) { s = StringIO.new(+"ab"); s.putc(""); s.string }
t(:r_closed_p) { r.closed? }
t(:rb_write) { StringIO.new(+"ab", "rb").write("x") }
w = StringIO.new(+"ab", "w")
t(:w_write) { w.write("xy") }
t(:w_read) { w.read }
t(:w_gets) { w.gets }
t(:w_eof) { w.eof? }
a = StringIO.new(+"ab", "a")
t(:a_write) { a.write("c"); a.string }
t(:a_read) { a.read }
t(:rplus) { s = StringIO.new(+"ab", "r+"); s.write("x"); s.rewind; s.read }
t(:wplus) { s = StringIO.new(+"ab", "w+"); s.write("xy"); s.rewind; s.read }
t(:aplus) { s = StringIO.new(+"ab", "a+"); s.write("c"); s.rewind; s.read }
t(:default) { s = StringIO.new; s.write("x"); s.rewind; s.read }

puts "-- string= keeps the sides"
c = closed_io
c.string = +"xy"
t(:closed_string_eq_read) { c.read }
t(:closed_string_eq_write) { c.write("Q") }
t(:closed_string_eq_closed_p) { c.closed? }
r2 = StringIO.new(+"ab", "r")
r2.string = +"cd"
t(:r_string_eq_write) { r2.write("Q") }
t(:r_string_eq_read) { r2.read }
w2 = StringIO.new(+"ab", "w")
w2.string = +"cd"
t(:w_string_eq_read) { w2.read }
t(:w_string_eq_write) { w2.write("Q"); w2.string }

puts "-- a subclass's super(str, mode)"
class Buf < StringIO
  def initialize(s, m) = super(s, m)
end
b = Buf.new(+"ab", "r")
t(:sub_r_write) { b.write("x") }
t(:sub_r_read) { b.read }
