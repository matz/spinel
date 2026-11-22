# spinel: not-cruby
# A Deflate#finish whose deflation fails leaves the stream as it was: not
# finished, its input still buffered, so a second finish answers the stream.
# The same holds for a flush. The failure (the native deflate answers nil
# when it cannot allocate) is made here by a deflate that fails on demand,
# which is why this is not a CRuby program.
require "zlib"

module Zlib
  FAIL = [0]
  module Native
    class << self
      alias_method :deflate_real, :deflate
      alias_method :deflate_block_real, :deflate_block
      def deflate(str, level, window_bits)
        if Zlib::FAIL[0] > 0
          Zlib::FAIL[0] -= 1
          return nil
        end
        deflate_real(str, level, window_bits)
      end

      def deflate_block(str, level, final)
        if Zlib::FAIL[0] > 0
          Zlib::FAIL[0] -= 1
          return nil
        end
        deflate_block_real(str, level, final)
      end
    end
  end
end

def attempt
  yield
rescue Zlib::Error => e
  puts "raised #{e.class}"
  nil
end

# finish with nothing flushed before it
d = Zlib::Deflate.new
d << "hello " << "world"
Zlib::FAIL[0] = 1
attempt { d.finish }
p d.finished?, d.total_in
out = d.finish
p d.finished?, d.total_in
p Zlib.inflate(out)
p d.finish

# a flush that fails, then one that works, then a finish that fails once
d = Zlib::Deflate.new
d << "abc"
Zlib::FAIL[0] = 1
attempt { d.flush }
p d.total_in
out = d.flush
d << "def"
Zlib::FAIL[0] = 1
attempt { d.finish }
p d.finished?, d.total_in
out << d.finish
p d.finished?, d.total_in
p Zlib.inflate(out)
