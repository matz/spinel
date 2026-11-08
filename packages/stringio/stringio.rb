# Spinel bundled `stringio` -- a carried-C spin package (Path B typed object).
#
# StringIO is a native-bound class: the struct and every method live in this
# package's C (sp_stringio.c, linked only when `require "stringio"` appears),
# and the declarations below are the compiler's entire knowledge of it. The
# compiler registers StringIO as a native class (a first-class object type:
# poly-capable, GC-managed, cls_id-dispatched) and emits direct typed calls to
# the declared symbols. Constructors receive the assigned cls_id first.
#
#   native_struct "Name", "c_struct"[, "finalizer"]
#   native_subclass "header", "scan"           (the class may be subclassed)
#   native_new    [arg_specs], "csym"          (arity-keyed; several allowed)
#   native_method :name, [arg_specs], ret, "csym"
#   specs: any | string | string? (nil-able) | int | float | bool | nil
#   a trailing rest takes every further argument, boxed: the C symbol gets a
#   count and an sp_RbVal array after the fixed arguments
#   native_subclass lets the program subclass the class: an instance of the
#   subclass embeds the struct, which the header defines, ahead of its own
#   instance variables; its scan calls the package's first, and each
#   constructor has an in-place twin (its name with `_new` read `_init`,
#   taking the object first) that the subclass's `super` in initialize calls
module StringIOPackage
  native_lib "stringio"
  native_obj "packages/stringio/sp_stringio.o"

  native_struct "StringIO", "sp_StringIO", "sp_StringIO_free"
  native_subclass "packages/stringio/sp_stringio.h", "sp_StringIO_scan_gc"
  native_new [],                 "sp_StringIO_new"
  native_new [:string],          "sp_StringIO_new_s"
  native_new [:string, :string], "sp_StringIO_new_sm"

  native_method :string,   [], :string,  "sp_StringIO_string"
  native_method :pos,      [], :int,     "sp_StringIO_pos"
  native_method :pos=,     [:int], :int, "sp_StringIO_set_pos"
  native_method :tell,     [], :int,     "sp_StringIO_tell"
  native_method :size,     [], :int,     "sp_StringIO_size"
  native_method :length,   [], :int,     "sp_StringIO_size"
  native_method :lineno,   [], :int,     "sp_StringIO_lineno"
  # :text -- a write payload: a String passes through, anything else is
  # written as its #to_s, as IO#write does
  native_method :write,    [:text], :int,   "sp_StringIO_write"
  native_method :write,    [:rest], :int, "sp_StringIO_write_va"
  native_method :<<,       [:text], :self,  "sp_StringIO_shl"
  native_method :puts,     [], :nil,     "sp_StringIO_puts_empty"
  native_method :puts,     [:string], :nil, "sp_StringIO_puts"
  native_method :puts,     [:any], :nil,  "sp_StringIO_puts_v1"
  native_method :puts,     [:any, :any], :nil, "sp_StringIO_puts_v2"
  native_method :puts,     [:any, :any, :any], :nil, "sp_StringIO_puts_v3"
  native_method :puts,     [:rest], :nil, "sp_StringIO_puts_va"
  native_method :print,    [:string], :nil, "sp_StringIO_print"
  native_method :print,    [:any], :nil,  "sp_StringIO_print_v1"
  native_method :print,    [:any, :any], :nil, "sp_StringIO_print_v2"
  native_method :print,    [:any, :any, :any], :nil, "sp_StringIO_print_v3"
  native_method :print,    [:rest], :nil, "sp_StringIO_print_va"
  native_method :putc,     [:int], :int, "sp_StringIO_putc_i"
  native_method :putc,     [:string], :string, "sp_StringIO_putc_s"
  native_method :putc,     [:any], :any, "sp_StringIO_putc_v"
  native_method :flush,    [], :self,    "sp_StringIO_flush"
  native_method :read,     [], :string,  "sp_StringIO_read"
  native_method :read,     [:int], :string, "sp_StringIO_read_n"
  native_method :read,     [:rest], :string, "sp_StringIO_read_va"
  native_method :gets,     [], :string?, "sp_StringIO_gets"
  native_method :gets,     [:string], :string?, "sp_StringIO_gets_sep"
  # the separator, the limit and `chomp:` (a keyword Hash boxes as the last
  # argument), decoded at run time: a lone Integer is the limit
  native_method :gets,     [:any], :string?, "sp_StringIO_gets_a1"
  native_method :gets,     [:any, :any], :string?, "sp_StringIO_gets_a2"
  native_method :gets,     [:any, :any, :any], :string?, "sp_StringIO_gets_a3"
  native_method :readline, [], :string,  "sp_StringIO_readline"
  native_method :readline, [:any], :string, "sp_StringIO_readline_a1"
  native_method :readline, [:any, :any], :string, "sp_StringIO_readline_a2"
  native_method :readline, [:any, :any, :any], :string, "sp_StringIO_readline_a3"
  native_method :readlines, [], :any,    "sp_StringIO_readlines"
  native_method :readlines, [:any], :any, "sp_StringIO_readlines_a1"
  native_method :readlines, [:any, :any], :any, "sp_StringIO_readlines_a2"
  native_method :readlines, [:any, :any, :any], :any, "sp_StringIO_readlines_a3"
  native_method :getc,     [], :string?, "sp_StringIO_getc"
  native_method :getbyte,  [], :any,     "sp_StringIO_getbyte"
  native_method :readbyte, [], :int,     "sp_StringIO_readbyte"
  native_method :readchar, [], :string,  "sp_StringIO_readchar"
  native_method :rewind,   [], :int,     "sp_StringIO_rewind"
  native_method :seek,     [:int], :int, "sp_StringIO_seek"
  native_method :seek,     [:int, :int], :int, "sp_StringIO_seek2"
  native_method :truncate, [:int], :int, "sp_StringIO_truncate"
  native_method :eof?,     [], :bool,    "sp_StringIO_eof_p"
  native_method :eof,      [], :bool,    "sp_StringIO_eof_p"
  native_method :close,    [], :void,    "sp_StringIO_close"
  native_method :closed?,  [], :bool,    "sp_StringIO_closed_p"
  native_method :sync,     [], :bool,    "sp_StringIO_sync"
  native_method :isatty,   [], :bool,    "sp_StringIO_isatty"
  native_method :tty?,     [], :bool,    "sp_StringIO_isatty"
  native_method :fsync,    [], :int,     "sp_StringIO_zero"
  native_method :fileno,   [], :int,     "sp_StringIO_zero"
  native_method :pid,      [], :int,     "sp_StringIO_zero"
end

# StringIO.open: sugar over .new -- plain Ruby, no compiler knowledge needed.
# With a block, yields the io and returns the block's value; without, acts
# as .new.
class StringIO
  def self.open(init = nil, mode = nil)
    io = if mode
      StringIO.new(init, mode)
    elsif init
      StringIO.new(init)
    else
      StringIO.new
    end
    if block_given?
      yield io
    else
      io
    end
  end

  # readpartial / sysread / read_nonblock: read(n), but EOFError at the end
  # of the string, as CRuby's StringIO does. read_nonblock answers nil there
  # instead when given exception: false. A given buffer is emptied at the end.
  # Neither passes its buffer on to the other: that changed how the buffer
  # parameter is passed, and the caller's buffer stopped seeing the replace.
  def readpartial(maxlen, outbuf = nil)
    raise ArgumentError, "negative length #{maxlen} given" if maxlen < 0
    s = maxlen == 0 ? "" : read(maxlen)
    if s.nil?
      outbuf.replace("") if outbuf
      raise EOFError, "end of file reached"
    end
    return s unless outbuf
    outbuf.replace(s)
    outbuf
  end
  alias sysread readpartial

  def read_nonblock(maxlen, outbuf = nil, exception: true)
    raise FrozenError, "can't modify frozen StringIO" if frozen?
    raise ArgumentError, "negative length #{maxlen} given" if maxlen < 0
    s = maxlen == 0 ? "" : read(maxlen)
    if s.nil?
      outbuf.replace("") if outbuf
      raise EOFError, "end of file reached" if exception
      return nil
    end
    return s unless outbuf
    outbuf.replace(s)
    outbuf
  end

  # The iteration surface, plain Ruby over gets / getc / getbyte. The BLOCK
  # form only: CRuby answers an Enumerator when no block is given, and a
  # method returning either that or `self` is a union spinel's typing has no
  # slot for -- `readlines.each` / `each_char.to_a` say the same thing here.
  def each_line(sep = "\n", limit = nil, chomp: false)
    # new locals: the spliced parameters would follow a block that rebinds the
    # variables passed in
    s = sep.is_a?(String) ? sep.to_s : sep
    l = limit
    # a lone Integer (or Float) is the limit, as it is for gets
    if l.nil? && (s.is_a?(Integer) || s.is_a?(Float))
      l = s.to_i
      s = "\n"
    end
    l = l.to_i if l.is_a?(Float)
    raise ArgumentError, "invalid limit: 0 for each_line" if l == 0
    if l.nil? && !chomp
      while (line = gets(s))
        yield line
      end
    else
      while (line = gets(s, l, chomp: chomp))
        yield line
      end
    end
    self
  end

  def each(sep = "\n", limit = nil, chomp: false, &blk)
    each_line(sep, limit, chomp: chomp, &blk)
  end


  def each_char
    while (ch = getc)
      yield ch
    end
    self
  end

  def each_byte
    while !eof?
      yield getbyte
    end
    self
  end
end
