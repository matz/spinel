# Spinel bundled `zlib` -- DEFLATE (RFC 1951), zlib (RFC 1950) and gzip
# (RFC 1952) over the package's own C (sp_zlib.c).
#
# What is here: Zlib.inflate / .deflate and the Inflate / Deflate objects,
# GzipReader / GzipWriter over a whole file, crc32 and adler32.
#
# What is absent is absent the way a subset is:
#
# * inflate is ONE-SHOT. `<<` accumulates and `finish` runs the whole
#   transform, so a caller that feeds a stream in pieces gets the right answer
#   but not incremental output, and one that never calls finish (or the block
#   form, or #inflate with the last chunk) gets nothing. A true streaming
#   inflater keeps a 32K window across calls; this one keeps the input.
# * deflate writes a block per flush: Deflate#deflate with SYNC_FLUSH or
#   FULL_FLUSH, Deflate#flush and GzipWriter#flush answer what was written
#   so far, ending on a byte. Each block starts with an empty window (what
#   zlib calls FULL_FLUSH), so a match never reaches back past a flush.
# * deflate emits fixed-Huffman blocks. The output is a valid stream every
#   inflater reads, at a ratio below zlib's own, which builds a dynamic table
#   per block. Zlib::BEST_COMPRESSION buys a deeper match search, not a
#   different format.
# * no preset dictionaries, no Zlib::ZStream introspection (total_in,
#   total_out, #avail_in), no #set_dictionary, no #params.
# * GzipReader reads a whole member; a multi-member .gz stops after the first,
#   as CRuby's does without #each_entry.
module Zlib
  MAX_WBITS = 15

  NO_COMPRESSION      = 0
  BEST_SPEED          = 1
  BEST_COMPRESSION    = 9
  DEFAULT_COMPRESSION = -1

  # Accepted and ignored: see the note above.
  NO_FLUSH      = 0
  SYNC_FLUSH    = 2
  FULL_FLUSH    = 3
  FINISH        = 4

  DEF_MEM_LEVEL = 8
  MAX_MEM_LEVEL = 9
  FILTERED      = 1
  HUFFMAN_ONLY  = 2
  DEFAULT_STRATEGY = 0

  class Error < StandardError; end
  class StreamEnd < Error; end
  class NeedDict < Error; end
  class DataError < Error; end
  class StreamError < Error; end
  class MemError < Error; end
  class BufError < Error; end
  class VersionError < Error; end
  class GzipFile < Error; end

  module Native
    native_lib "zlib"
    native_obj "packages/zlib/sp_zlib.o"
    # :string return: the C side builds a length-set BINARY string on the GC
    # heap, so an embedded NUL survives and no strlen is taken of it.
    native_func :inflate,    [:string, :int],       :string, "sp_zlib_inflate"
    native_func :deflate,    [:string, :int, :int], :string, "sp_zlib_deflate"
    native_func :deflate_block, [:string, :int, :int], :string, "sp_zlib_deflate_block"
    native_func :crc32,      [:string, :any],       :any,    "sp_zlib_crc32_of"
    native_func :adler32,    [:string, :any],       :any,    "sp_zlib_adler32_of"
    native_func :last_error, [],                    :cstring, "sp_zlib_last_error"
  end

  module_function

  def crc32(str = "", crc = 0)
    Native.crc32(str, crc)
  end

  def adler32(str = "", adler = 1)
    Native.adler32(str, adler)
  end

  def zlib_version
    "1.2.11"
  end

  # A malformed stream answers nil from the C side; the message it left behind
  # is the one zlib would have given, so the exception reads the same.
  def raw_inflate(str, window_bits)
    out = Native.inflate(str, window_bits)
    raise DataError, Native.last_error if out.nil?
    out
  end

  def raw_deflate(str, level, window_bits)
    out = Native.deflate(str, level, window_bits)
    raise Error, Native.last_error if out.nil?
    out
  end

  def inflate(str)
    raw_inflate(str, MAX_WBITS)
  end

  def deflate(str, level = DEFAULT_COMPRESSION)
    raw_deflate(str, level, MAX_WBITS)
  end

  def gunzip(str)
    raw_inflate(str, MAX_WBITS + 16)
  end

  def gzip(str, level: DEFAULT_COMPRESSION, strategy: nil)
    raw_deflate(str, level, MAX_WBITS + 16)
  end

  # Inflate and Deflate share the accumulate-then-transform shape, and differ
  # only in which direction `run` goes. Keeping them separate classes (rather
  # than one with a flag) is what lets each carry CRuby's own class methods.
  class Inflate
    def self.inflate(str)
      Zlib.raw_inflate(str, Zlib::MAX_WBITS)
    end

    def initialize(window_bits = Zlib::MAX_WBITS)
      @window_bits = window_bits
      @buf = String.new
      @finished = false
      @closed = false
    end

    def <<(str)
      @buf << str.to_s
      self
    end

    # CRuby's #inflate returns what came out of THIS call. One-shot means all
    # of it comes out of the last one, so a caller that feeds everything and
    # concatenates the returns gets the right bytes in the right order.
    def inflate(str = nil)
      @buf << str.to_s unless str.nil?
      out = Zlib.raw_inflate(@buf, @window_bits)
      @finished = true
      out
    end

    def finish
      return String.new if @finished
      inflate(nil)
    end

    def total_in
      @buf.bytesize
    end

    def finished?
      @finished
    end

    def close
      @closed = true
      nil
    end

    def closed?
      @closed
    end
  end

  # A stream never flushed is compressed whole at finish, as one block. Once
  # flushed, each flush writes the input since the last one as a block of its
  # own, and this side writes the wrapper the blocks go in: the zlib or gzip
  # header before the first, the checksum and length after the last.
  class Deflate
    def self.deflate(str, level = Zlib::DEFAULT_COMPRESSION)
      Zlib.raw_deflate(str, level, Zlib::MAX_WBITS)
    end

    def initialize(level = Zlib::DEFAULT_COMPRESSION, window_bits = Zlib::MAX_WBITS,
                   mem_level = Zlib::DEF_MEM_LEVEL, strategy = Zlib::DEFAULT_STRATEGY)
      @level = level
      @window_bits = window_bits
      @buf = String.new
      @finished = false
      @closed = false
      @started = false
      @total_in = 0
      @adler = 1
      @crc = 0
    end

    def <<(str)
      s = str.to_s
      raise StreamError, "stream error" if @finished && !s.empty?
      @buf << s
      self
    end

    # What this call wrote: nothing for NO_FLUSH, the input so far as a block
    # for SYNC_FLUSH and FULL_FLUSH, the end of the stream for FINISH.
    def deflate(str = nil, mode = Zlib::NO_FLUSH)
      return after_finish(str, mode) if @finished
      @buf << str.to_s unless str.nil?
      flush(mode)
    end

    # A SYNC_FLUSH with nothing new since the last flush is a BufError, as
    # zlib answers Z_BUF_ERROR to a call that cannot make progress; a
    # FULL_FLUSH writes the empty block all the same. Past finish, a flush is
    # a StreamError, as is #deflate.
    def flush(mode = Zlib::SYNC_FLUSH)
      return finish if mode == Zlib::FINISH
      return String.new if mode == Zlib::NO_FLUSH
      raise StreamError, "stream error" if @finished
      raise BufError, "buffer error" if mode == Zlib::SYNC_FLUSH && @started && @buf.empty?
      out = @started ? String.new : header
      @started = true
      out << block(0)
      out
    end

    def finish
      return String.new if @finished
      @finished = true
      unless @started
        data = @buf
        @buf = String.new
        @total_in = data.bytesize
        return Zlib.raw_deflate(data, @level, @window_bits)
      end
      out = block(1)
      out << trailer
      out
    end

    def total_in
      @total_in + @buf.bytesize
    end

    def finished?
      @finished
    end

    def close
      @closed = true
      nil
    end

    def closed?
      @closed
    end

    private

    # Past finish, zlib takes no more input: a BufError at FINISH, a
    # StreamError otherwise. With no input only a flush is refused, and
    # with none given at all nothing is.
    def after_finish(str, mode)
      return String.new if str.nil?
      s = str.to_s
      raise BufError, "buffer error" if mode == Zlib::FINISH && !s.empty?
      raise StreamError, "stream error" unless s.empty? && (mode == Zlib::NO_FLUSH || mode == Zlib::FINISH)
      String.new
    end

    # The input since the last flush as one block, BFINAL set when final
    # is 1; the checksum moves on over it.
    def block(final)
      data = @buf
      @buf = String.new
      @total_in += data.bytesize
      if gzip?
        @crc = Zlib.crc32(data, @crc)
      elsif !raw?
        @adler = Zlib.adler32(data, @adler)
      end
      out = Native.deflate_block(data, @level, final)
      raise Error, Native.last_error if out.nil?
      out
    end

    # The window_bits of zlib: negative raw deflate, +16 gzip, +32 either
    # (zlib when writing).
    def raw?
      @window_bits < 0
    end

    def gzip?
      @window_bits >= 24 && @window_bits < 40
    end

    # The bytes the C side writes before a whole stream, so a stream in
    # pieces starts the same way.
    def header
      return String.new if raw?
      return [0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff].pack("C*") if gzip?
      [0x78, 0x9c].pack("C*")
    end

    def trailer
      return String.new if raw?
      return [@crc, @total_in & 0xffffffff].pack("VV") if gzip?
      [@adler].pack("N")
    end
  end

  # GzipReader / GzipWriter over a whole file. CRuby's take an IO; these take
  # a path or an IO, because the block form on a path is what nearly every
  # caller writes and the IO form is one read away.
  class GzipReader
    # The block's value is carried in a local rather than left as the value of
    # `begin ... ensure ... end`. Both are the same Ruby; the second form is
    # one spinel gets wrong today -- a yielding method whose TAIL is a begin/
    # ensure has its return type pinned by the first call site, so a second
    # call with a block of another type reads the first one's slot. Minimal:
    #   def run; begin; yield 7; ensure; nil; end; end
    #   p run { |x| x == 7 }   #=> true
    #   p run { |x| x * 3 }    #=> true, where CRuby says 21
    def self.open(path)
      r = new(File.open(path.to_s, "rb"))
      return r unless block_given?
      result = nil
      begin
        result = yield r
      ensure
        r.close
      end
      result
    end

    def initialize(io)
      @io = io
      @data = nil
      @pos = 0
    end

    def data
      @data ||= Zlib.raw_inflate(@io.read, Zlib::MAX_WBITS + 16)
    end

    def read(len = nil)
      d = data
      return nil if @pos >= d.bytesize && !len.nil?
      if len.nil?
        out = d.byteslice(@pos, d.bytesize - @pos)
        @pos = d.bytesize
        return out
      end
      out = d.byteslice(@pos, len)
      @pos += out.bytesize
      out
    end

    def eof?
      @pos >= data.bytesize
    end

    def each_line
      data.each_line { |l| yield l }
      nil
    end

    def readlines
      data.lines
    end

    def close
      @io.close
      nil
    end
  end

  class GzipWriter
    def self.open(path, level = Zlib::DEFAULT_COMPRESSION)
      w = new(File.open(path.to_s, "wb"), level)
      return w unless block_given?
      result = nil
      begin
        result = yield w
      ensure
        w.close
      end
      result
    end

    def initialize(io, level = Zlib::DEFAULT_COMPRESSION)
      @io = io
      @deflate = Deflate.new(level, Zlib::MAX_WBITS + 16)
    end

    def write(str)
      s = str.to_s
      @deflate << s
      s.bytesize
    end

    def <<(str)
      @deflate << str.to_s
      self
    end

    # Writes to the IO what was written so far, as a block that ends on a
    # byte: a reader of the IO can inflate it before the end.
    def flush(mode = Zlib::SYNC_FLUSH)
      @io.write(@deflate.flush(mode)) unless mode == Zlib::NO_FLUSH
      self
    end

    def puts(str = "")
      write("#{str}\n")
      nil
    end

    def print(*args)
      args.each { |a| write(a.to_s) }
      nil
    end

    def close
      @io.write(@deflate.finish)
      @io.close
      nil
    end
  end
end
