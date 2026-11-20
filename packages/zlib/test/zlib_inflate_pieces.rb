# Zlib::Inflate over a stream that comes in pieces: each piece answers the
# output it completes, wherever the stream is cut.
#
# The sizes a call answers are not pinned. What is pinned: the pieces join
# to the whole text, output comes before the last piece does, and the end
# of the stream is seen when its trailer is in. The three streams written
# out below are zlib's own, since Zlib.deflate here writes neither a
# dynamic-Huffman block nor a stored one.
require "zlib"

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

# Feeds `stream` to `inflate` in pieces of `step` bytes: the joined output,
# and whether any of it came before the last piece.
def feed(inflate, stream, step)
  out = String.new
  early = false
  pos = 0
  while pos < stream.bytesize
    out << inflate.inflate(stream.byteslice(pos, step))
    pos += step
    early = true if pos < stream.bytesize && !out.empty?
  end
  [out, early]
end

def show(label)
  puts "#{label}: #{yield.inspect}"
rescue Zlib::Error => e
  puts "#{label}: #{e.class}: #{e.message}"
end

# Only the class of a malformed block: its message is zlib's own wording.
def show_class(label)
  puts "#{label}: #{yield.inspect}"
rescue Zlib::Error => e
  puts "#{label}: #{e.class}"
end

fox = (1..24).map { |k| "row #{k}: the quick brown fox #{k * k} jumps over the lazy dog\n" }.join
dynamic = ["78da8594491602210c44f79e2247e884a1c1db38b4b3a268dbeae9f5b9a6923dff85aa7ca865229e" \
           "d36337d06ddcaf8eb4ac65bad0a6bc88e9309eaf772acfa1fe0f9c169f37adcb76f63b42d286bc0a" \
           "b9369455c883eb45950a6d4a824ac536e5f4593de842cf95da54d42bcc6d2ae9dbe20e94d8753a87" \
           "dc10631ed083bd9e8e1d5ab6de252349b2be39469a189e70447a19f3802a928c7c401627469f1909" \
           "6dbc6ee08b377c11e08bf7c63cf49d243d9f005f82e87d0af025f4787f5f1997c2fe"].pack("H*")
named = ["1f8b0818010000000203666f782e74787400726f7773008594491602210c44f79e2247e884a1c1db" \
         "38b4b3a268dbeae9f5b9a6923dff85aa7ca865229ed36337d06ddcaf8eb4ac65bad0a6bc88e9309e" \
         "af772acfa1fe0f9c169f37adcb76f63b42d286bc0ab9369455c883eb45950a6d4a824ac536e5f459" \
         "3de842cf95da54d42bcc6d2ae9dbe20e94d8753a87dc10631ed083bd9e8e1d5ab6de252349b2be39" \
         "469a189e70447a19f3802a928c7c401627469f19096dbc6ee08b377c11e08bf7c63cf49d243d9f00" \
         "5f82e87d0af025f4787f5f6339f29c2b050000"].pack("H*")
stored = ["7801010c00f3ff73746f7265642062797465731fcf04d9"].pack("H*")

[["dynamic", dynamic, Zlib::MAX_WBITS, fox],
 ["gzip with a name", named, Zlib::MAX_WBITS + 16, fox],
 ["gzip found by its header", named, Zlib::MAX_WBITS + 32, fox],
 ["zlib found by its header", dynamic, Zlib::MAX_WBITS + 32, fox],
 ["stored", stored, Zlib::MAX_WBITS, "stored bytes"]].each do |label, stream, window_bits, text|
  [1, 2, 3, 7, 64, 1000].each do |step|
    i = Zlib::Inflate.new(window_bits)
    out, early = feed(i, stream, step)
    puts "#{label} by #{step}: whole #{out == text} early #{early} finished #{i.finished?} " \
         "total_in #{i.total_in == stream.bytesize} rest #{i.finish.inspect}"
  end
end

# Matches that reach back across pieces, as far as the window goes, and
# more output than the window holds.
big = noise(30_000, 3) + ("abcdefgh" * 9000) + noise(30_000, 3) + ("spinel " * 3000) + noise(2_000, 7)
[["zlib", Zlib::MAX_WBITS], ["gzip", Zlib::MAX_WBITS + 16], ["raw", -Zlib::MAX_WBITS]].each do |label, window_bits|
  d = Zlib::Deflate.new(Zlib::DEFAULT_COMPRESSION, window_bits)
  stream = d.deflate(big, Zlib::FINISH)
  [5, 997, 16_384].each do |step|
    i = Zlib::Inflate.new(window_bits)
    out, early = feed(i, stream, step)
    puts "#{label} past the window by #{step}: whole #{out == big} early #{early} finished #{i.finished?}"
  end
end

# A stream written with a flush after each piece gives each piece back as
# its bytes come in.
d = Zlib::Deflate.new
parts = ["one ", "two ", "three"].map { |piece| d.deflate(piece, Zlib::SYNC_FLUSH) }
parts << d.finish
i = Zlib::Inflate.new
puts "a flushed stream: #{parts.map { |part| i.inflate(part) }.inspect} finished #{i.finished?}"

z = Zlib.deflate(fox)
gz = Zlib.gzip(fox)

# << runs the stream too; the output waits for #inflate or #finish.
show("<< then finish") { i = Zlib::Inflate.new; i << z; [i.finished?, i.finish == fox, i.finish] }
show("<< then inflate") { i = Zlib::Inflate.new; i << z.byteslice(0, 40); i.inflate(z.byteslice(40, z.bytesize)) == fox }
show("<< then inflate(nil)") { i = Zlib::Inflate.new; i << z; i.inflate(nil) == fox }
show("an empty piece") { i = Zlib::Inflate.new; [i.inflate(""), i.finished?, i.total_in] }
show("total_in on the way") { i = Zlib::Inflate.new; i << z.byteslice(0, 20); i.total_in }

# A stream that has not ended is a BufError at finish, and goes on after it.
show("finish with nothing") { Zlib::Inflate.new.finish }
show("inflate(nil) with nothing") { Zlib::Inflate.new.inflate(nil) }
show("finish before the end") { i = Zlib::Inflate.new; i << z.byteslice(0, 30); i.finish }
show("and on after it") do
  i = Zlib::Inflate.new
  i << z.byteslice(0, 30)
  begin
    i.finish
  rescue Zlib::BufError
  end
  [i.inflate(z.byteslice(30, z.bytesize)) == fox, i.finished?]
end
show("the trailer still to come") do
  i = Zlib::Inflate.new
  [i.inflate(z.byteslice(0, z.bytesize - 4)) == fox, i.finished?, i.inflate(z.byteslice(z.bytesize - 4, 4)), i.finished?]
end
show("gzip, the trailer still to come") do
  i = Zlib::Inflate.new(Zlib::MAX_WBITS + 16)
  [i.inflate(gz.byteslice(0, gz.bytesize - 3)) == fox, i.finished?]
end

# Past the end of the stream the input is kept, and #finish answers it.
show("input after the end") { i = Zlib::Inflate.new; i.inflate(z); [i.inflate("more"), i.finish, i.finish, i.total_in == z.bytesize] }
show("twice, then inflate(nil)") { i = Zlib::Inflate.new; i.inflate(z); i.inflate("a"); i << "b"; [i.inflate(nil), i.inflate(nil)] }
show("after the end in one piece") { i = Zlib::Inflate.new; out = i.inflate(z + "extra"); [out == fox, i.finished?, i.total_in == z.bytesize, i.finish] }
show("after the end, then more") { i = Zlib::Inflate.new; i.inflate(z + "extra"); [i.inflate("x"), i.finish] }
show("<< past the end") { i = Zlib::Inflate.new; i << z; i << "tail"; [i.inflate(""), i.finish == fox + "tail"] }
show("a second gzip member") { i = Zlib::Inflate.new(Zlib::MAX_WBITS + 16); [i.inflate(gz + gz) == fox, i.finish == gz] }
show("raw, after the end") { i = Zlib::Inflate.new(-Zlib::MAX_WBITS); d = Zlib::Deflate.new(6, -Zlib::MAX_WBITS); [i.inflate(d.deflate("raw", Zlib::FINISH) + "zz"), i.finish] }

# What is wrong with a header or a trailer is told as soon as it is in.
def flip(stream, at)
  bad = stream.dup
  bad.setbyte(at, bad.getbyte(at) ^ 1)
  bad
end

show("not a zlib header") { Zlib::Inflate.new.inflate("xx") }
show("one byte of a header") { i = Zlib::Inflate.new; [i.inflate("x"), i.finished?] }
show("and again after the error") do
  i = Zlib::Inflate.new
  begin
    i.inflate("xx")
  rescue Zlib::DataError
  end
  i.inflate(z)
end
show("not deflate") { Zlib::Inflate.new.inflate([0x77, 0x09].pack("C*")) }
show("a window too large") { Zlib::Inflate.new(8).inflate(dynamic) }
show("the window the header names") { [0, 32].map { |bits| Zlib::Inflate.new(bits).inflate(dynamic) == fox } + [Zlib::Inflate.new(32).inflate(named) == fox] }
show("gzip to a zlib inflater") { Zlib::Inflate.new.inflate(gz) }
show("zlib to a gzip inflater") { Zlib::Inflate.new(Zlib::MAX_WBITS + 16).inflate(z) }
show("half a gzip magic") { i = Zlib::Inflate.new(Zlib::MAX_WBITS + 16); [i.inflate([0x1f].pack("C")), i.inflate([0x8b].pack("C"))] }
show("gzip, not deflate") { Zlib::Inflate.new(Zlib::MAX_WBITS + 16).inflate([0x1f, 0x8b, 7, 0].pack("C*")) }
show("gzip, unknown flags") { Zlib::Inflate.new(Zlib::MAX_WBITS + 16).inflate([0x1f, 0x8b, 8, 0x20].pack("C*")) }
show("a wrong adler32") { Zlib::Inflate.new.inflate(flip(z, z.bytesize - 1)) }
show("a wrong adler32, in pieces") do
  i = Zlib::Inflate.new
  bad = flip(z, z.bytesize - 1)
  out = i.inflate(bad.byteslice(0, bad.bytesize - 2))
  puts "  the text came first: #{out == fox}"
  i.inflate(bad.byteslice(bad.bytesize - 2, 2))
end
show("a wrong crc32") { Zlib::Inflate.new(Zlib::MAX_WBITS + 16).inflate(flip(gz, gz.bytesize - 5)) }
show("a wrong length") { Zlib::Inflate.new(Zlib::MAX_WBITS + 16).inflate(flip(gz, gz.bytesize - 1)) }
show_class("a block of no type") { Zlib::Inflate.new.inflate(z.byteslice(0, 2) + [0xff, 0xff, 0xff, 0xff].pack("C*")) }
show_class("a stored block with a wrong length") { Zlib::Inflate.new.inflate(flip(stored, 3)) }
show_class("a match before the start") { Zlib::Inflate.new(-Zlib::MAX_WBITS).inflate([0x4b, 0x04, 0x42, 0x00].pack("C*")) }

# A closed stream answers nothing more.
show("closed?") { i = Zlib::Inflate.new; before = i.closed?; [before, i.close, i.closed?] }
show("inflate when closed") { i = Zlib::Inflate.new; i.close; i.inflate(z) }
show("<< when closed") { i = Zlib::Inflate.new; i.close; i << z; 1 }
show("finish when closed") { i = Zlib::Inflate.new; i.inflate(z); i.close; i.finish }
show("finished? when closed") { i = Zlib::Inflate.new; i.close; i.finished? }
show("total_in when closed") { i = Zlib::Inflate.new; i.close; i.total_in }
show("close when closed") { i = Zlib::Inflate.new; i.close; i.close }

# A whole stream at once is as it was.
show("a whole stream") { [Zlib.inflate(z) == fox, Zlib::Inflate.inflate(z) == fox, Zlib.gunzip(gz) == fox] }
