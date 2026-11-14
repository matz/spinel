# Spinel bundled `io/buffer` -- IO::Buffer as a native binding with NO
# carried C: the implementation lives in the runtime (lib/sp_iobuffer.c),
# and the declarations below are the compiler's knowledge of it. The
# compiler splices this file into any program that references `IO::Buffer`
# (the implicit-require treatment `Set` gets), so the class needs no
# explicit require, as in CRuby.
#
# The declared surface is CRuby 4.x's in-memory API plus the IO
# integration: read/write/pread/pwrite against an IO handle and
# IO::Buffer.map over a file (#4474).
module IOBufferPackage
  native_lib "io/buffer"

  native_struct "IO::Buffer", "sp_IOBuffer", "sp_IOBuffer_fin"
  native_new [],            "sp_IOBuffer_new"
  native_new [:int],        "sp_IOBuffer_new_i"
  native_new [:int, :int],  "sp_IOBuffer_new_if"

  native_method :size,      [], :int,    "sp_IOBuffer_size"
  native_method :get_value, [:any, :int], :any, "sp_IOBuffer_get_value"
  native_method :set_value, [:any, :int, :any], :int, "sp_IOBuffer_set_value"
  native_method :get_string, [], :string, "sp_IOBuffer_get_string0"
  native_method :get_string, [:int], :string, "sp_IOBuffer_get_string1"
  native_method :get_string, [:int, :int], :string, "sp_IOBuffer_get_string2"
  native_method :set_string, [:string], :int, "sp_IOBuffer_set_string1"
  native_method :set_string, [:string, :int], :int, "sp_IOBuffer_set_string2"
  native_method :set_string, [:string, :int, :int], :int, "sp_IOBuffer_set_string3"
  native_method :set_string, [:string, :int, :int, :int], :int, "sp_IOBuffer_set_string4"
  native_method :resize,    [:int], :self, "sp_IOBuffer_resize"
  native_method :clear,     [], :self, "sp_IOBuffer_clear0"
  native_method :clear,     [:int], :self, "sp_IOBuffer_clear1"
  native_method :clear,     [:int, :int], :self, "sp_IOBuffer_clear2"
  native_method :clear,     [:int, :int, :int], :self, "sp_IOBuffer_clear3"
  native_method :copy,      [:any], :int, "sp_IOBuffer_copy1"
  native_method :copy,      [:any, :int], :int, "sp_IOBuffer_copy2"
  native_method :copy,      [:any, :int, :int], :int, "sp_IOBuffer_copy3"
  native_method :copy,      [:any, :int, :int, :int], :int, "sp_IOBuffer_copy4"
  native_method :slice,     [], :self, "sp_IOBuffer_slice0"
  native_method :slice,     [:int], :self, "sp_IOBuffer_slice1"
  native_method :slice,     [:int, :int], :self, "sp_IOBuffer_slice2"
  native_method :transfer,  [], :self, "sp_IOBuffer_transfer"
  native_method :free,      [], :self, "sp_IOBuffer_free_m"
  native_method :dup,       [], :self, "sp_IOBuffer_dup_m"
  native_method :clone,     [], :self, "sp_IOBuffer_dup_m"
  native_method :<=>,       [:any], :int, "sp_IOBuffer_cmp"
  native_method :==,        [:any], :bool, "sp_IOBuffer_eq"
  native_method :hexdump,   [], :string?, "sp_IOBuffer_hexdump0"
  native_method :hexdump,   [:int], :string?, "sp_IOBuffer_hexdump1"
  native_method :hexdump,   [:int, :int], :string?, "sp_IOBuffer_hexdump2"
  native_method :hexdump,   [:int, :int, :int], :string?, "sp_IOBuffer_hexdump3"
  native_method :inspect,   [], :string, "sp_IOBuffer_inspect"
  native_method :to_s,      [], :string, "sp_IOBuffer_to_s"
  native_method :null?,     [], :bool, "sp_IOBuffer_null_p"
  native_method :empty?,    [], :bool, "sp_IOBuffer_empty_p"
  native_method :valid?,    [], :bool, "sp_IOBuffer_valid_p"
  native_method :external?, [], :bool, "sp_IOBuffer_external_p"
  native_method :internal?, [], :bool, "sp_IOBuffer_internal_p"
  native_method :mapped?,   [], :bool, "sp_IOBuffer_mapped_p"
  native_method :shared?,   [], :bool, "sp_IOBuffer_shared_p"
  native_method :locked?,   [], :bool, "sp_IOBuffer_locked_p"
  native_method :readonly?, [], :bool, "sp_IOBuffer_readonly_p"
  native_method :private?,  [], :bool, "sp_IOBuffer_private_p"
  native_method :&,         [:any], :self, "sp_IOBuffer_and"
  native_method :|,         [:any], :self, "sp_IOBuffer_or"
  native_method :^,         [:any], :self, "sp_IOBuffer_xor"
  native_method :~,         [], :self, "sp_IOBuffer_not"
  native_method :and!,      [:any], :self, "sp_IOBuffer_and_ip"
  native_method :or!,       [:any], :self, "sp_IOBuffer_or_ip"
  native_method :xor!,      [:any], :self, "sp_IOBuffer_xor_ip"
  native_method :not!,      [], :self, "sp_IOBuffer_not_ip"
  # Typed accessors behind the compiler's literal-symbol get_value/set_value
  # lowering (codegen_call_recv.c). The int argument is the type-table index
  # (lib/sp_iobuffer.h's SP_IOB_TY_* / src/compiler.c's comp_iob_sym_type).
  native_method :__get_i,   [:int, :int], :int, "sp_IOBuffer_get_i"
  native_method :__get_f,   [:int, :int], :float, "sp_IOBuffer_get_f"
  native_method :__get_x,   [:int, :int], :any, "sp_IOBuffer_get_x"
  native_method :__set_i,   [:int, :int, :int], :int, "sp_IOBuffer_set_i"
  native_method :__set_f,   [:int, :int, :float], :int, "sp_IOBuffer_set_f"
  native_method :__set_v,   [:int, :int, :any], :int, "sp_IOBuffer_set_v"
  native_method :__check_writable, [], :self, "sp_IOBuffer_check_writable"
  native_method :__lock,    [], :self, "sp_IOBuffer_lock"
  native_method :__unlock,  [], :self, "sp_IOBuffer_unlock"
  native_method :__become_for, [:string], :self, "sp_IOBuffer_become_for"
  native_method :__borrow_string, [:string], :self, "sp_IOBuffer_borrow_string"
  native_method :__end_borrow, [], :self, "sp_IOBuffer_end_borrow"
  # The IO integration: the Ruby-side read/write/pread/pwrite below hold the
  # lock around these; a negative length or size stands for nil.
  native_method :__read_io,   [:any, :int, :int], :int, "sp_IOBuffer_read_io"
  native_method :__write_io,  [:any, :int, :int], :int, "sp_IOBuffer_write_io"
  native_method :__pread_io,  [:any, :int, :int, :int], :int, "sp_IOBuffer_pread_io"
  native_method :__pwrite_io, [:any, :int, :int, :int], :int, "sp_IOBuffer_pwrite_io"
  native_method :__become_map, [:any, :int, :int, :int], :self, "sp_IOBuffer_become_map"

  native_func :__page_size, [], :int, "sp_IOBuffer_page_size"
end

# The Ruby side: constants, the block-taking surface (locked, each, ...),
# and the class methods that compose the native primitives.
class IO::Buffer
  EXTERNAL = 1
  INTERNAL = 2
  MAPPED = 4
  SHARED = 8
  LOCKED = 32
  PRIVATE = 64
  READONLY = 128
  LITTLE_ENDIAN = 4
  BIG_ENDIAN = 8
  HOST_ENDIAN = 4
  NETWORK_ENDIAN = 8
  DEFAULT_SIZE = 65536
  PAGE_SIZE = IOBufferPackage.__page_size

  # With a block, lend the String's current bytes for the duration of
  # the yield. The non-block form keeps its read-only snapshot.
  def self.for(string)
    return new(0).__become_for(string) unless block_given?

    owner = new(0)
    buffer = owner.__borrow_string(string)
    begin
      yield buffer
    ensure
      owner.__end_borrow
      buffer.free
    end
  end

  # IO::Buffer.map(file, size = nil, offset = 0, flags = 0): a view of the
  # file through mmap (munmap'd by the finalizer; resize refused). As in
  # CRuby the default is a writable shared mapping, so a file opened
  # read-only needs READONLY.
  def self.map(file, size = nil, offset = 0, flags = 0)
    new(0).__become_map(file, size.nil? ? -1 : size, offset, flags)
  end

  def self.string(length)
    raise LocalJumpError, "no block given" unless block_given?
    buffer = new(length)
    yield buffer
    result = buffer.get_string
    result
  end

  def self.size_of(type)
    if type.is_a?(Array)
      total = 0
      type.each { |t| total += size_of(t) }
      return total
    end
    case type
    when :U8, :S8 then 1
    when :u16, :s16, :U16, :S16 then 2
    when :u32, :s32, :U32, :S32, :f32, :F32 then 4
    when :u64, :s64, :U64, :S64, :f64, :F64 then 8
    else
      raise ArgumentError, "Invalid type name!"
    end
  end

  # The BLOCK form only: without a block CRuby answers LocalJumpError. The
  # lock is taken first (an already locked buffer raises LockedError), and
  # released on the way out, by an exception or a missing block too, as
  # Ruby 4.0.7 does; 4.0.4 left it locked.
  def locked
    __lock
    begin
      raise LocalJumpError, "no block given" unless block_given?
      yield self
    ensure
      __unlock
    end
  end

  # One read(2) / write(2) / pread(2) / pwrite(2) against `io`, answering
  # the byte count, 0 at EOF, or -errno, as CRuby does. The buffer is locked
  # for the duration (a blocking read parks the thread; nothing may move the
  # bytes meanwhile) and unlocked on the way out, exception or not.
  def read(io, length = nil, offset = 0)
    __lock
    begin
      __read_io(io, length.nil? ? -1 : length, offset)
    ensure
      __unlock
    end
  end

  def write(io, length = nil, offset = 0)
    __lock
    begin
      __write_io(io, length.nil? ? -1 : length, offset)
    ensure
      __unlock
    end
  end

  def pread(io, from, length = nil, offset = 0)
    __lock
    begin
      __pread_io(io, from, length.nil? ? -1 : length, offset)
    ensure
      __unlock
    end
  end

  def pwrite(io, from, length = nil, offset = 0)
    __lock
    begin
      __pwrite_io(io, from, length.nil? ? -1 : length, offset)
    ensure
      __unlock
    end
  end

  def each(type = :U8, offset = 0, count = nil)
    width = IO::Buffer.size_of(type)
    raise ArgumentError, "Offset can't be negative!" if offset < 0
    raise ArgumentError, "The given offset is bigger than the buffer size!" if offset > size
    n = 0
    while offset <= size - width
      break if count && n >= count
      yield offset, get_value(type, offset)
      offset += width
      n += 1
    end
    self
  end

  def each_byte(offset = 0, count = nil)
    raise ArgumentError, "Offset can't be negative!" if offset < 0
    raise ArgumentError, "The given offset is bigger than the buffer size!" if offset > size
    n = 0
    while offset < size
      break if count && n >= count
      yield get_value(:U8, offset)
      offset += 1
      n += 1
    end
    self
  end

  def values(type = :U8, offset = 0, count = nil)
    acc = []
    each(type, offset, count) { |_off, v| acc << v }
    acc
  end

  def get_values(types, offset)
    unless types.is_a?(Array)
      raise ArgumentError, "Argument buffer_types should be an array!"
    end
    acc = []
    types.each do |t|
      acc << get_value(t, offset)
      offset += IO::Buffer.size_of(t)
    end
    acc
  end

  # Answers the offset past the last value, as set_value does. The buffer
  # is checked first (read-only, an invalidated slice), then the types, the
  # offset (so an empty list at a negative one raises too) and the values,
  # in CRuby's order.
  def set_values(types, offset, values)
    __check_writable
    unless types.is_a?(Array)
      raise ArgumentError, "Argument buffer_types should be an array!"
    end
    raise ArgumentError, "Offset can't be negative!" if offset < 0
    unless values.is_a?(Array)
      raise ArgumentError, "Argument values should be an array!"
    end
    if types.size != values.size
      raise ArgumentError, "Argument buffer_types and values should have the same length!"
    end
    i = 0
    while i < types.size
      offset = set_value(types[i], offset, values[i])
      i += 1
    end
    offset
  end
end
