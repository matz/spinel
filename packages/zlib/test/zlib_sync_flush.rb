# Zlib::Deflate in pieces: #deflate with SYNC_FLUSH or FULL_FLUSH, and
# #flush, answer what was written so far; NO_FLUSH answers nothing; FINISH
# ends the stream. GzipWriter#flush writes the same to its IO.
#
# The compressed bytes are not pinned (zlib_test.rb says why). What is
# pinned: each flush answers bytes; the stream round-trips in the zlib, gzip
# and raw framings; and every flush ends on a byte with all the input so far
# in it, which is what lets a reader inflate a stream before its end. That
# is checked by closing the deflate data seen so far with an empty final
# block (03 00) and inflating it raw.
require "zlib"
require "stringio"

# Bytes with nothing in them to match, the same under every Ruby.
def noise(n, seed)
  x = seed
  out = []
  n.times do
    x = (x * 75 + 74) % 65537
    out << (x % 256).chr
  end
  out.join
end

# The deflate data of a stream: the bytes after the zlib or gzip header.
# (By bytes: a StringIO's string is UTF-8, and the gzip mtime can read as
# a character.)
def body_of(stream, framing)
  return stream if framing == "raw"
  skip = framing == "gzip" ? 10 : 2
  stream.byteslice(skip, stream.bytesize - skip)
end

def inflater(framing)
  return Zlib::Inflate.new(-Zlib::MAX_WBITS) if framing == "raw"
  return Zlib::Inflate.new(Zlib::MAX_WBITS + 16) if framing == "gzip"
  Zlib::Inflate.new
end

def window_bits(framing)
  return -Zlib::MAX_WBITS if framing == "raw"
  return Zlib::MAX_WBITS + 16 if framing == "gzip"
  Zlib::MAX_WBITS
end

text = "<p>" + ("spinel " * 300) + "</p>\n"
cases = [
  ["one byte", ["x"]],
  ["text", ["<html>\n", text, "<p>" + ("ruby " * 300) + "</p>\n", "</html>\n"]],
  ["past 64 KB", [noise(40_000, 3), ("abcdefgh" * 9000), noise(30_000, 7)]],
]

%w[zlib gzip raw].each do |framing|
  cases.each do |label, pieces|
    [Zlib::DEFAULT_COMPRESSION, 0, 9].each do |level|
      d = Zlib::Deflate.new(level, window_bits(framing))
      stream = String.new
      sent = String.new
      answers = []
      prefixes = []
      pieces.each_with_index do |piece, i|
        out = i.even? ? d.deflate(piece, Zlib::SYNC_FLUSH) : (d << piece; d.flush)
        answers << (out.bytesize > 0)
        stream << out
        sent << piece
        closed = body_of(stream, framing) + [0x03, 0x00].pack("C*")
        prefixes << (Zlib::Inflate.new(-Zlib::MAX_WBITS).inflate(closed) == sent)
      end
      stream << d.finish
      whole = inflater(framing).inflate(stream) == sent
      puts "#{framing} #{label} level #{level}: answers #{answers.inspect} prefixes #{prefixes.inspect} " \
           "round trip #{whole} total_in #{d.total_in == sent.bytesize}"
    end
  end
end

# A flush with nothing new: the first writes the header and an empty
# block; a SYNC_FLUSH after a flush is a BufError, as zlib answers; a
# FULL_FLUSH writes the empty stored block (00 00 00 ff ff). After finish,
# new input is refused, as is a flush.
def attempt(stream, label)
  out = yield
  stream << out
  puts "#{label}: #{out.unpack1("H*")}"
rescue Zlib::Error => e
  puts "#{label}: #{e.class}"
end

d = Zlib::Deflate.new
stream = String.new
attempt(stream, "first flush, nothing in") { d.flush }
attempt(stream, "SYNC_FLUSH, nothing new") { d.deflate("", Zlib::SYNC_FLUSH) }
attempt(stream, "flush, nothing new") { d.flush }
attempt(stream, "FULL_FLUSH, nothing new") { d.deflate("", Zlib::FULL_FLUSH) }
attempt(stream, "NO_FLUSH with input") { d.deflate("abc", Zlib::NO_FLUSH) }
out = d.deflate("def", Zlib::SYNC_FLUSH)
stream << out << d.finish
puts "then SYNC_FLUSH answers #{out.bytesize > 0}; the stream inflates to #{Zlib.inflate(stream).inspect}"
attempt(stream, "flush after finish") { d.flush }
attempt(stream, "deflate after finish") { d.deflate("x") }

# Past finish, new input is refused: a BufError at FINISH, a StreamError
# otherwise. Each case on a stream of its own.
def finished_deflate
  d = Zlib::Deflate.new
  d.finish
  d
end

[nil, "", "x"].each do |s|
  [["FINISH", Zlib::FINISH], ["FULL_FLUSH", Zlib::FULL_FLUSH], ["SYNC_FLUSH", Zlib::SYNC_FLUSH],
   ["NO_FLUSH", Zlib::NO_FLUSH]].each do |name, mode|
    attempt(String.new, "after finish, deflate(#{s.inspect}, #{name})") { finished_deflate.deflate(s, mode) }
  end
end
attempt(String.new, "after finish, << \"\"") { finished_deflate << ""; String.new }
attempt(String.new, "after finish, << \"x\"") { finished_deflate << "x"; String.new }

# Never flushed: finish writes the stream Zlib.deflate writes.
d = Zlib::Deflate.new
d << text
puts "never flushed is Zlib.deflate: #{d.finish == Zlib.deflate(text)} total_in #{d.total_in == text.bytesize}"

io = StringIO.new
gz = Zlib::GzipWriter.new(io)
sizes = []
sent = String.new
prefixes = []
["<html>\n", text, "</html>\n"].each do |piece|
  gz.write(piece)
  sent << piece
  gz.flush
  sizes << io.string.bytesize
  closed = body_of(io.string, "gzip") + [0x03, 0x00].pack("C*")
  prefixes << (Zlib::Inflate.new(-Zlib::MAX_WBITS).inflate(closed) == sent)
end
gz.close
# GzipWriter#flush flushes the IO too, in every mode.
class CountingIO < StringIO
  def flushes = @flushes || 0

  def flush
    @flushes = flushes + 1
    self
  end
end

counted = CountingIO.new
gz2 = Zlib::GzipWriter.new(counted)
gz2.write("a")
gz2.flush
gz2.flush(Zlib::NO_FLUSH)
puts "GzipWriter#flush flushes the IO: #{counted.flushes}"

puts "GzipWriter#flush: grows #{sizes.each_cons(2).all? { |a, b| b > a } && sizes[0] > 10} prefixes #{prefixes.inspect} " \
     "round trip #{Zlib.gunzip(io.string) == sent}"
