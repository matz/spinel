# Spinel bundled `ffi` -- the ffi gem's API, so gems written against CRuby's
# `ffi` compile unchanged.
#
# The gem is a C extension over libffi; here the API is Ruby (the way
# TruffleRuby carries it) over sp_ffi.c, which is the libffi and dlopen glue
# and nothing else. Libraries are opened and symbols resolved at run time, and
# a call's signature is whatever the Ruby side built -- so `ffi_lib` takes a
# computed path, struct layouts hold function pointers that are called later,
# and callbacks are ordinary procs.
#
# Addresses are Integers on this side. A Pointer is an object holding one; it
# is never handed to the collector as something to follow.
#
# What the compiler does for this file's users: `attach_function :name, ...`
# in a module that extends FFI::Library also defines `name` on that module
# (and as an instance method, for modules that are included), because a
# method's name has to be known when the program is compiled. The attach
# itself -- symbol lookup, NotFoundError -- still happens when the module body
# runs, as in CRuby.
require "ffi/accessors"

module FFI
  VERSION = "1.17.0"

  # ---- the native layer (packages/ffi/sp_ffi.c) ----
  module Native
    native_lib "ffi"
    native_obj "packages/ffi/sp_ffi.o"
    ffi_lib "ffi"
    native_func :dlopen,       [:string, :int],        :int,     "sp_ffi_dlopen"
    native_func :dlerror,      [],                     :cstring, "sp_ffi_dlerror"
    native_func :dlsym,        [:int, :string],        :int,     "sp_ffi_dlsym"
    native_func :dlclose,      [:int],                 :int,     "sp_ffi_dlclose"
    native_func :rtld,         [:int],                 :int,     "sp_ffi_rtld"
    native_func :malloc,       [:int],                 :int,     "sp_ffi_malloc"
    native_func :calloc,       [:int],                 :int,     "sp_ffi_calloc"
    native_func :realloc,      [:int, :int],           :int,     "sp_ffi_realloc"
    native_func :free,         [:int],                 :void,    "sp_ffi_free"
    native_func :memcpy,       [:int, :int, :int],     :void,    "sp_ffi_memcpy"
    native_func :memset,       [:int, :int, :int],     :void,    "sp_ffi_memset"
    native_func :strlen,       [:int],                 :int,     "sp_ffi_strlen"
    native_func :strnlen,      [:int, :int],           :int,     "sp_ffi_strnlen"
    native_func :str_addr,     [:string],              :int,     "sp_ffi_str_addr"
    native_func :read_bytes,   [:int, :int],           :cbinstr, "sp_ffi_read_bytes"
    native_func :write_bytes,  [:int, :string, :int, :int], :void, "sp_ffi_write_bytes"
    native_func :zero_bytes,   [:int],                 :cbinstr, "sp_ffi_zero_bytes"
    native_func :get_int,      [:int, :int],           :int,     "sp_ffi_get_int"
    native_func :put_int,      [:int, :int, :int],     :void,    "sp_ffi_put_int"
    native_func :get_float,    [:int, :int],           :float,   "sp_ffi_get_float"
    native_func :put_float,    [:int, :int, :float],   :void,    "sp_ffi_put_float"
    native_func :type_prim,    [:int],                 :int,     "sp_ffi_type_prim"
    native_func :type_struct,  [:int, :int],           :int,     "sp_ffi_type_struct"
    native_func :type_size,    [:int],                 :int,     "sp_ffi_type_size"
    native_func :type_align,   [:int],                 :int,     "sp_ffi_type_align"
    native_func :cif_new,      [:int, :int, :int, :int], :int,   "sp_ffi_cif_new"
    native_func :call,         [:int, :int, :int, :int], :void,  "sp_ffi_call"
    native_func :ret_int,      [:int, :int],           :int,     "sp_ffi_ret_int"
    native_func :put_ret_int,  [:int, :int, :int],     :void,    "sp_ffi_put_ret_int"
    native_func :closure_new,  [:int, :int],           :int,     "sp_ffi_closure_new"
    native_func :errno,        [],                     :int,     "sp_ffi_errno"
    native_func :set_errno,    [:int],                 :void,    "sp_ffi_set_errno"
    native_func :sizeof,       [:int],                 :int,     "sp_ffi_sizeof"
    native_func :little_endian, [],                    :int,     "sp_ffi_little_endian"
  end

  # The closure dispatcher is handed to C once, as a function pointer.
  module NativeCallback
    ffi_source <<~C
      #include <stdint.h>
      void sp_ffi_set_dispatcher(void (*)(int64_t, int64_t, int64_t));
    C
    ffi_callback :dispatch_fn, [:long, :long, :long], :void
    ffi_func :sp_ffi_set_dispatcher, [:dispatch_fn], :void
  end

  class NotFoundError < LoadError; end
  class NullPointerError < RuntimeError; end
  class SignatureError < RuntimeError; end
  class TypeError < ::TypeError; end

  # ---- platform ----
  module Platform
    OS = if RUBY_PLATFORM.include?("darwin") then "darwin"
         elsif RUBY_PLATFORM.include?("linux") then "linux"
         elsif RUBY_PLATFORM.include?("freebsd") then "freebsd"
         elsif RUBY_PLATFORM.include?("openbsd") then "openbsd"
         elsif RUBY_PLATFORM.include?("netbsd") then "netbsd"
         else "unknown"
         end
    ARCH = if RUBY_PLATFORM.include?("aarch64") || RUBY_PLATFORM.include?("arm64") then "aarch64"
           elsif RUBY_PLATFORM.include?("x86_64") then "x86_64"
           else RUBY_PLATFORM.split("-").first
           end
    NAME = "#{ARCH}-#{OS}"
    IS_MAC = OS == "darwin"
    IS_LINUX = OS == "linux"
    IS_WINDOWS = false
    IS_FREEBSD = OS == "freebsd"
    IS_OPENBSD = OS == "openbsd"
    IS_NETBSD = OS == "netbsd"
    IS_BSD = IS_MAC || IS_FREEBSD || IS_OPENBSD || IS_NETBSD
    IS_GNU = IS_LINUX
    LIBPREFIX = "lib"
    LIBSUFFIX = IS_MAC ? "dylib" : "so"
    LIBC = IS_MAC ? "libc.dylib" : (IS_LINUX ? "libc.so.6" : "libc.so")
    ADDRESS_SIZE = 64
    LONG_SIZE = 64
    ADDRESS_ALIGN = 8
    BIG_ENDIAN = 4321
    LITTLE_ENDIAN = 1234
    BYTE_ORDER = Native.little_endian == 1 ? LITTLE_ENDIAN : BIG_ENDIAN
    CPU = ARCH

    def self.windows? = false
    def self.mac? = IS_MAC
    def self.linux? = IS_LINUX
    def self.unix? = true
    def self.bsd? = IS_BSD
    def self.solaris? = false
    def self.is_os(os) = OS == os.to_s
  end

  # ---- types ----
  class Type
    K_VOID = 0
    K_I8 = 1
    K_U8 = 2
    K_I16 = 3
    K_U16 = 4
    K_I32 = 5
    K_U32 = 6
    K_I64 = 7
    K_U64 = 8
    K_F32 = 9
    K_F64 = 10
    K_PTR = 11
    K_BOOL = 12
    K_STRUCT = 13   # an aggregate: a struct, a union or an inline array

    attr_reader :size, :alignment, :kind, :name

    def initialize(kind, size, alignment, name)
      @kind = kind
      @size = size
      @alignment = alignment
      @name = name
      @ffi_type = 0
    end

    def ffi_type
      @ffi_type = Native.type_prim(@kind) if @ffi_type == 0
      @ffi_type
    end

    def native_type = self
    def inspect = "#<FFI::Type:#{@name} size=#{@size} alignment=#{@alignment}>"
    def to_s = inspect
    def float? = @kind == K_F32 || @kind == K_F64
    def integer? = (@kind >= K_I8 && @kind <= K_U64) || @kind == K_BOOL
    def pointer? = @kind == K_PTR
    def void? = @kind == K_VOID

    # nil converts to no number: the ffi gem's NUM2INT / NUM2DBL raise, where
    # nil.to_i / nil.to_f would pass 0 / 0.0 to C
    def self.int_arg(v)
      if v.is_a?(Integer) then v
      elsif v.nil? then raise ::TypeError, "no implicit conversion from nil to integer"
      elsif v == true then 1
      elsif v == false then 0
      elsif v.is_a?(Float) then v.to_i
      elsif v.respond_to?(:to_int) then v.to_int
      else raise ::TypeError, "no implicit conversion of #{v.class} into Integer"
      end
    end
    def self.float_arg(v)
      raise ::TypeError, "no implicit conversion to float from nil" if v.nil?
      v.to_f
    end

    # Store `v` as this type's native representation at `addr`. `keep` collects
    # Ruby objects whose memory C reads (strings) so they outlive the call; a
    # store into memory passes nil, and a String there is refused.
    def write_native(addr, v, keep)
      if integer?
        Native.put_int(addr, @kind, Type.int_arg(v))
      elsif float?
        Native.put_float(addr, @kind, Type.float_arg(v))
      elsif pointer?
        Native.put_int(addr, K_PTR, FFI.pointer_address(v, keep))
      end
      nil
    end

    # The Ruby value of the native representation at `addr`.
    def read_native(addr)
      if @kind == K_BOOL then Native.get_int(addr, K_U8) != 0
      elsif integer? then Native.get_int(addr, @kind)
      elsif float? then Native.get_float(addr, @kind)
      elsif pointer? then Pointer.new(Native.get_int(addr, K_PTR))
      else nil
      end
    end

    # A return value: small integers come back register-widened.
    def read_return(addr)
      if @kind == K_BOOL then Native.ret_int(addr, K_BOOL) != 0
      elsif integer? then Native.ret_int(addr, @kind)
      else read_native(addr)
      end
    end

    def write_return(addr, v, keep)
      if integer? then Native.put_ret_int(addr, @kind, Type.int_arg(v))
      else write_native(addr, v, keep)
      end
      nil
    end

    # the native type a by-value slot of this type needs, in bytes
    def slot_size = @size < 8 ? 8 : @size

    class Builtin < Type; end

    # :string -- a char * that reads back as a Ruby String (nil for NULL)
    class StringType < Type
      def initialize = super(K_PTR, 8, 8, "STRING")
      def write_native(addr, v, keep)
        # as the gem: memory would keep no String alive, so it takes none
        raise ::ArgumentError, "Cannot set :string fields" unless keep
        a = if v.nil? then 0
            elsif v.is_a?(String)
              # the String's own bytes, as the gem passes them: a pointer the
              # callee keeps (strtol's endptr) stays inside the caller's String
              keep << v
              Native.str_addr(v)
            else FFI.pointer_address(v, keep)
            end
        Native.put_int(addr, K_PTR, a)
        nil
      end
      def read_native(addr)
        p = Native.get_int(addr, K_PTR)
        p == 0 ? nil : Native.read_bytes(p, Native.strlen(p))
      end
      def read_return(addr) = read_native(addr)
    end

    # :strptr -- a returned char * as [String, Pointer], so the caller can free it
    class StrPtrType < Type
      def initialize = super(K_PTR, 8, 8, "STRPTR")
      def read_native(addr)
        p = Native.get_int(addr, K_PTR)
        [p == 0 ? nil : Native.read_bytes(p, Native.strlen(p)), Pointer.new(p)]
      end
      def read_return(addr) = read_native(addr)
    end

    # A typedef of a DataConverter: converts on the way in and out.
    class Mapped < Type
      attr_reader :converter
      def initialize(converter)
        @converter = converter
        nt = FFI.find_type(converter.native_type)
        super(nt.kind, nt.size, nt.alignment, "MAPPED")
        @native = nt
      end
      def native_type = @native
      def to_native(v, ctx) = @converter.to_native(v, ctx)
      def from_native(v, ctx) = @converter.from_native(v, ctx)
      def write_native(addr, v, keep) = @native.write_native(addr, @converter.to_native(v, nil), keep)
      def read_native(addr) = @converter.from_native(@native.read_native(addr), nil)
      def read_return(addr) = @converter.from_native(@native.read_return(addr), nil)
      def write_return(addr, v, keep) = @native.write_return(addr, @converter.to_native(v, nil), keep)
    end
  end

  class Type
    VOID = Builtin.new(K_VOID, 1, 1, "VOID")
    BOOL = Builtin.new(K_BOOL, 1, 1, "BOOL")
    INT8 = Builtin.new(K_I8, 1, 1, "INT8")
    UINT8 = Builtin.new(K_U8, 1, 1, "UINT8")
    INT16 = Builtin.new(K_I16, 2, 2, "INT16")
    UINT16 = Builtin.new(K_U16, 2, 2, "UINT16")
    INT32 = Builtin.new(K_I32, 4, 4, "INT32")
    UINT32 = Builtin.new(K_U32, 4, 4, "UINT32")
    INT64 = Builtin.new(K_I64, 8, 8, "INT64")
    UINT64 = Builtin.new(K_U64, 8, 8, "UINT64")
    FLOAT32 = Builtin.new(K_F32, 4, 4, "FLOAT32")
    FLOAT64 = Builtin.new(K_F64, 8, 8, "FLOAT64")
    POINTER = Builtin.new(K_PTR, 8, 8, "POINTER")
    STRING = StringType.new
    STRPTR = StrPtrType.new
    CHAR = INT8
    UCHAR = UINT8
    SHORT = INT16
    USHORT = UINT16
    INT = INT32
    UINT = UINT32
    LONG = INT64
    ULONG = UINT64
    LONG_LONG = INT64
    ULONG_LONG = UINT64
    FLOAT = FLOAT32
    DOUBLE = FLOAT64
    LONGDOUBLE = FLOAT64
    BUFFER_IN = POINTER
    BUFFER_OUT = POINTER
    BUFFER_INOUT = POINTER
    VARARGS = Builtin.new(K_VOID, 0, 1, "VARARGS")
  end

  NativeType = Type

  TYPE_VOID = Type::VOID
  TYPE_INT8 = Type::INT8
  TYPE_UINT8 = Type::UINT8
  TYPE_INT16 = Type::INT16
  TYPE_UINT16 = Type::UINT16
  TYPE_INT32 = Type::INT32
  TYPE_UINT32 = Type::UINT32
  TYPE_INT64 = Type::INT64
  TYPE_UINT64 = Type::UINT64
  TYPE_FLOAT32 = Type::FLOAT32
  TYPE_FLOAT64 = Type::FLOAT64
  TYPE_BOOL = Type::BOOL
  TYPE_STRING = Type::STRING

  BUILTIN_TYPES = {
    void: Type::VOID, bool: Type::BOOL,
    char: Type::INT8, uchar: Type::UINT8, int8: Type::INT8, uint8: Type::UINT8,
    short: Type::INT16, ushort: Type::UINT16, int16: Type::INT16, uint16: Type::UINT16,
    int: Type::INT32, uint: Type::UINT32, int32: Type::INT32, uint32: Type::UINT32,
    long: Type::INT64, ulong: Type::UINT64, int64: Type::INT64, uint64: Type::UINT64,
    long_long: Type::INT64, ulong_long: Type::UINT64,
    float: Type::FLOAT32, double: Type::FLOAT64, float32: Type::FLOAT32, float64: Type::FLOAT64,
    long_double: Type::FLOAT64,
    pointer: Type::POINTER, string: Type::STRING, strptr: Type::STRPTR,
    buffer_in: Type::POINTER, buffer_out: Type::POINTER, buffer_inout: Type::POINTER,
    varargs: Type::VARARGS,
    size_t: Type::UINT64, ssize_t: Type::INT64, intptr_t: Type::INT64, uintptr_t: Type::UINT64,
    ptrdiff_t: Type::INT64, off_t: Type::INT64, time_t: Type::INT64, pid_t: Type::INT32,
    uid_t: Type::UINT32, gid_t: Type::UINT32, mode_t: (Platform::IS_MAC ? Type::UINT16 : Type::UINT32),
    dev_t: (Platform::IS_MAC ? Type::INT32 : Type::UINT64), ino_t: Type::UINT64,
    socklen_t: Type::UINT32, in_addr_t: Type::UINT32, in_port_t: Type::UINT16,
    wchar_t: Type::INT32, blksize_t: Type::INT64, blkcnt_t: Type::INT64,
    nlink_t: (Platform::IS_MAC ? Type::UINT16 : Type::UINT64),
    clock_t: Type::UINT64, suseconds_t: Type::INT32, useconds_t: Type::UINT32,
    int8_t: Type::INT8, uint8_t: Type::UINT8, int16_t: Type::INT16, uint16_t: Type::UINT16,
    int32_t: Type::INT32, uint32_t: Type::UINT32, int64_t: Type::INT64, uint64_t: Type::UINT64,
    u_int8_t: Type::UINT8, u_int16_t: Type::UINT16, u_int32_t: Type::UINT32, u_int64_t: Type::UINT64,
    caddr_t: Type::POINTER, sa_family_t: Type::UINT8, fsblkcnt_t: Type::UINT64, fsfilcnt_t: Type::UINT64,
    rlim_t: Type::UINT64, id_t: Type::UINT32, key_t: Type::INT32, clockid_t: Type::UINT32,
  }

  # Typedefs made through FFI.typedef and every Library module's typedef, by
  # name. A Struct layout looks names up here, since it has no module scope.
  TYPEDEFS = {}

  # An inline struct or array field read out of `owner`'s memory is a view of
  # it, not a copy: its pointer keeps the owner (and so the bytes) alive.
  def self.__view_of(v, owner)
    if v.is_a?(Struct) || v.is_a?(InlineArray)
      v.to_ptr.__owned_by(owner)
    end
    v
  end

  def self.find_type(name, type_map = nil)
    if name.is_a?(Type) then name
    elsif name.is_a?(Symbol)
      t = type_map ? type_map[name] : nil
      t ||= TYPEDEFS[name]
      t ||= BUILTIN_TYPES[name]
      raise ::TypeError, "unable to resolve type '#{name}'" unless t
      t
    elsif name.is_a?(String)
      find_type(name.to_sym, type_map)
    elsif name.nil?
      Type::VOID
    elsif name.is_a?(Class) && name <= Struct
      name.by_ref
    elsif name.is_a?(Module) || name.respond_to?(:native_type)
      # a DataConverter: a module (or class) answering native_type /
      # to_native / from_native
      Type::Mapped.new(name)
    else
      raise ::TypeError, "unable to resolve type '#{name}'"
    end
  end

  def self.type_size(t) = find_type(t).size

  def self.typedef(old, add)
    TYPEDEFS[add] = find_type(old)
  end

  def self.add_typedef(old, add) = typedef(old, add)

  def self.errno = Native.errno
  def self.errno=(v)
    Native.set_errno(v)
  end

  module LastError
    def self.error = Native.errno
    def self.error=(v)
      Native.set_errno(v)
    end
    def self.winapi_error = 0
  end

  # The address a value stands for when C wants a pointer.
  def self.pointer_address(v, keep)
    if v.nil? then 0
    elsif v.is_a?(Integer) then v
    elsif v.is_a?(AbstractMemory) then v.address
    elsif v.is_a?(Struct) then v.pointer.address
    elsif v.is_a?(String)
      # a call's argument lives for the call; memory would outlive the String
      raise ::ArgumentError, "value is not a pointer" unless keep
      keep << v
      Native.str_addr(v)
    elsif v.respond_to?(:to_ptr) then pointer_address(v.to_ptr, keep)
    else raise ::ArgumentError, "Invalid pointer value: #{v.inspect}"
    end
  end

  # ---- memory ----
  class AbstractMemory
    attr_reader :address

    LONG_MAX = 9_223_372_036_854_775_807

    def initialize(address = 0, size = nil, type_size = 1)
      @address = address
      @size = size
      @type_size = type_size
    end

    def size = @size || LONG_MAX
    def total = size

    # A view into memory another object owns (a MemoryPointer's buffer, or a
    # view of one) holds that object, so the bytes live as long as it does.
    def __owned_by(owner)
      @owner = owner
      self
    end

    # An object the memory at `addr` now points into (a MemoryPointer, a
    # Struct, a Function stored in a struct field): held by the owner of the
    # memory, per address, until the slot is written again -- as the gem's
    # struct keeps what its pointer fields hold. A view passes it on.
    def __retain(addr, v)
      o = @owner
      if o
        o.__retain(addr, v)
      else
        @retained ||= {}
        if v.is_a?(AbstractMemory) || v.is_a?(Struct) || v.is_a?(Proc) || v.is_a?(Method)
          @retained[addr] = v
        else
          @retained.delete(addr)
        end
      end
      nil
    end
    def type_size = @type_size
    def to_i = @address
    def null? = @address == 0
    def order(*_) = self
    def autorelease? = true
    def autorelease=(v)
    end

    def __addr(off, n)
      raise NullPointerError, "invalid memory access at address=0x0" if @address == 0
      if @size && (off < 0 || off + n > @size)
        raise IndexError, "Memory access offset=#{off} size=#{n} is out of bounds"
      end
      @address + off
    end

    def clear
      Native.memset(@address, 0, @size) if @size
      self
    end

    def get_pointer(off) = Pointer.new(Native.get_int(__addr(off, 8), Type::K_PTR))
    def put_pointer(off, v)
      Native.put_int(__addr(off, 8), Type::K_PTR, FFI.pointer_address(v, nil))
      self
    end
    def read_pointer = get_pointer(0)
    def write_pointer(v) = put_pointer(0, v)
    def get_array_of_pointer(off, count)
      a = []
      i = 0
      while i < count
        a << get_pointer(off + i * 8)
        i += 1
      end
      a
    end
    def put_array_of_pointer(off, ary)
      i = 0
      while i < ary.size
        put_pointer(off + i * 8, ary[i])
        i += 1
      end
      self
    end
    def read_array_of_pointer(count)
      a = []
      i = 0
      while i < count
        a << Pointer.new(Native.get_int(__addr(i * 8, 8), Type::K_PTR))
        i += 1
      end
      a
    end
    def write_array_of_pointer(ary) = put_array_of_pointer(0, ary)

    def get_bool(off) = Native.get_int(__addr(off, 1), Type::K_U8) != 0
    def put_bool(off, v)
      Native.put_int(__addr(off, 1), Type::K_U8, v ? 1 : 0)
      self
    end
    def read_bool = get_bool(0)
    def write_bool(v) = put_bool(0, v)

    def get_bytes(off, len)
      return "" if len == 0
      Native.read_bytes(__addr(off, len), len)
    end
    def put_bytes(off, str, index = 0, length = nil)
      len = length || (str.bytesize - index)
      raise IndexError, "Memory access offset=#{index} size=#{len} is out of bounds" if index < 0 || index + len > str.bytesize
      Native.write_bytes(__addr(off, len), str, index, len) if len > 0
      self
    end
    def read_bytes(len) = get_bytes(0, len)
    def write_bytes(str, index = 0, length = nil) = put_bytes(0, str, index, length)

    def get_string(off, len = nil)
      # a length past the end of a sized buffer is CRuby's IndexError
      a = __addr(off, len || 0)
      n = if len then Native.strnlen(a, len)
          elsif @size then Native.strnlen(a, @size - off)
          else Native.strlen(a)
          end
      Native.read_bytes(a, n)
    end
    def put_string(off, str)
      put_bytes(off, str)
      Native.put_int(__addr(off + str.bytesize, 1), Type::K_U8, 0)
      self
    end
    def read_string(len = nil)
      len ? get_bytes(0, len) : get_string(0)
    end
    def read_string_length(len) = get_bytes(0, len)
    def read_string_to_null = get_string(0)
    def write_string(str, len = nil)
      len ? put_bytes(0, str, 0, len) : put_string(0, str)
    end
    def write_string_length(str, len) = put_bytes(0, str, 0, len)
    def get_array_of_string(off, count = nil)
      a = []
      i = 0
      loop do
        break if count && i >= count
        p = Native.get_int(__addr(off + i * 8, 8), Type::K_PTR)
        break if count.nil? && p == 0
        a << (p == 0 ? nil : Native.read_bytes(p, Native.strlen(p)))
        i += 1
      end
      a
    end
    def read_array_of_string(count = nil) = get_array_of_string(0, count)

    # typed access by type name: get(:int, 4), put(:double, 0, 1.5)
    def get(type, off)
      t = FFI.find_type(type)
      FFI.__view_of(t.read_native(__addr(off, t.size)), self)
    end
    def put(type, off, v)
      t = FFI.find_type(type)
      t.write_native(__addr(off, t.size), v, nil)
      self
    end
    def read(type) = get(type, 0)
    def write(type, v) = put(type, 0, v)
    def read_array_of_type(type, reader, count)
      t = FFI.find_type(type)
      a = []
      i = 0
      while i < count
        a << FFI.__view_of(t.read_native(__addr(i * t.size, t.size)), self)
        i += 1
      end
      a
    end
    def write_array_of_type(type, writer, ary)
      t = FFI.find_type(type)
      i = 0
      while i < ary.size
        if writer == :put_string
          put_string(i * t.size, ary[i])   # the bytes themselves, as the gem's writer does
        else
          t.write_native(__addr(i * t.size, t.size), ary[i], nil)
        end
        i += 1
      end
      self
    end

    def __copy_from__(src, len)
      Native.memcpy(@address, src.address, len)
      self
    end
  end

  class Pointer < AbstractMemory
    SIZE = 8

    def self.size = 8

    # Pointer.new(address) / Pointer.new(type, address) / Pointer.new(pointer)
    def initialize(a = 0, b = nil)
      if b.nil?
        addr = a.is_a?(AbstractMemory) ? a.address : a
        super(addr, nil, 1)
      else
        addr = b.is_a?(AbstractMemory) ? b.address : b
        super(addr, nil, FFI.find_type(a).size)
      end
    end

    def +(off)
      Pointer.new(@address + off).__with_size(@size ? @size - off : nil, @type_size).__owned_by(self)
    end


    def __with_size(size, type_size)
      @size = size
      @type_size = type_size
      self
    end

    def slice(off, len) = Pointer.new(@address + off).__with_size(len, 1).__owned_by(self)

    def [](idx) = self + idx * @type_size

    def ==(other)
      if other.nil? then @address == 0
      elsif other.is_a?(AbstractMemory) then @address == other.address
      else false
      end
    end

    def eql?(other) = other.is_a?(AbstractMemory) && @address == other.address
    def hash = @address.hash

    def to_ptr = self
    def inspect = "#<FFI::Pointer address=0x#{@address.to_s(16)}>"
    def to_s = inspect
    def free
      Native.free(@address) if @address != 0
      @address = 0
      nil
    end
  end

  class Pointer
    NULL = Pointer.new(0)
  end

  # Memory the collector owns: the bytes are a GC String held by this object,
  # so they are freed when the pointer (and everything slicing it) is gone --
  # the gem's autorelease. The collector does not move objects, so the address
  # is stable for the pointer's life.
  class MemoryPointer < Pointer
    def initialize(size, count = 1, clear = true)
      tsize = if size.is_a?(Integer) then size
              elsif size.is_a?(Class) && size <= Struct then size.size   # the struct itself
              else FFI.find_type(size).size
              end
      total = tsize * (count || 1)
      @buffer = Native.zero_bytes(total)
      super(Native.str_addr(@buffer))
      @size = total
      @type_size = tsize
    end

    def self.from_string(s)
      mp = MemoryPointer.new(s.bytesize + 1)
      mp.put_bytes(0, s)
      mp
    end

    def free
      @address = 0
      nil
    end

    def inspect = "#<FFI::MemoryPointer address=0x#{@address.to_s(16)} size=#{@size}>"
  end

  class Buffer < MemoryPointer
    def self.alloc_in(size, count = 1, clear = true) = Buffer.new(size, count, clear)
    def self.alloc_out(size, count = 1, clear = true) = Buffer.new(size, count, clear)
    def self.alloc_inout(size, count = 1, clear = true) = Buffer.new(size, count, clear)
    def self.new_in(size, count = 1, clear = true) = Buffer.new(size, count, clear)
    def self.new_out(size, count = 1, clear = true) = Buffer.new(size, count, clear)
    def self.new_inout(size, count = 1, clear = true) = Buffer.new(size, count, clear)
  end

  # ---- enums ----
  class Enum < Type
    attr_reader :tag

    def initialize(info, tag = nil, native = nil)
      nt = native ? FFI.find_type(native) : Type::INT32
      super(nt.kind, nt.size, nt.alignment, "ENUM")
      @native = nt
      @tag = tag
      @kv = {}
      @vk = {}
      nxt = 0
      i = 0
      while i < info.size
        sym = info[i]
        if i + 1 < info.size && info[i + 1].is_a?(Integer)
          nxt = info[i + 1]
          i += 1
        end
        @kv[sym] = nxt
        @vk[nxt] = sym unless @vk.key?(nxt)
        nxt += 1
        i += 1
      end
    end

    def native_type = @native
    def symbols = @kv.keys
    def to_h = @kv
    def to_hash = @kv
    def symbol_map = @kv

    def [](q)
      if q.is_a?(Symbol) then @kv[q]
      elsif q.is_a?(Integer) then @vk[q]
      else nil
      end
    end
    def find(q) = self[q]

    def to_native(v, ctx)
      if v.is_a?(Symbol)
        n = @kv[v]
        raise ::ArgumentError, "invalid enum value, #{v.inspect}" unless n
        n
      elsif v.is_a?(Integer) then v
      elsif v.respond_to?(:to_int) then v.to_int
      else raise ::ArgumentError, "invalid enum value, #{v.inspect}"
      end
    end

    def from_native(v, ctx)
      s = @vk[v]
      s || v
    end

    def write_native(addr, v, keep) = @native.write_native(addr, to_native(v, nil), keep)
    def read_native(addr) = from_native(@native.read_native(addr), nil)
    def read_return(addr) = from_native(@native.read_return(addr), nil)
    def write_return(addr, v, keep) = @native.write_return(addr, to_native(v, nil), keep)
  end

  class Bitmask < Enum
    def initialize(info, tag = nil, native = nil)
      # bitmask info names bit positions unless given explicit values
      expanded = []
      bit = 0
      i = 0
      while i < info.size
        sym = info[i]
        if i + 1 < info.size && info[i + 1].is_a?(Integer)
          expanded << sym << info[i + 1]
          i += 2
        else
          expanded << sym << (1 << bit)
          bit += 1
          i += 1
        end
      end
      super(expanded, tag, native)
    end

    def to_native(v, ctx)
      if v.is_a?(Array)
        r = 0
        v.each { |s| r |= super(s, ctx) }
        r
      else
        super(v, ctx)
      end
    end

    def from_native(v, ctx)
      out = []
      @kv.each { |k, bits| out << k if bits != 0 && (v & bits) == bits }
      out
    end
  end

  class Enums
    def initialize
      @all = []
      @tagged = {}
    end
    def <<(e)
      @all << e
      @tagged[e.tag] = e if e.tag
      self
    end
    def find(q)
      return @tagged[q] if @tagged.key?(q)
      @all.each { |e| return e if e.symbols.include?(q) }
      nil
    end
    def __map_symbol(sym)
      @all.each { |e| v = e[sym]; return v if v }
      nil
    end
  end

  module DataConverter
    def native_type(t = nil)
      if t
        @native_type = FFI.find_type(t)
      else
        @native_type || Type::POINTER
      end
    end
    def to_native(v, ctx) = v
    def from_native(v, ctx) = v
  end

  # ---- functions ----
  #
  # A FunctionType is a signature: the argument types and the return type.
  # Used as a callback typedef it converts a proc into a C function pointer.
  class FunctionType < Type
    attr_reader :return_type, :param_types, :varargs

    def initialize(ret, params, options = nil)
      super(K_PTR, 8, 8, "CALLBACK")
      @return_type = FFI.find_type(ret)
      ps = []
      @varargs = false
      params.each do |p|
        t = FFI.find_type(p)
        if t.equal?(Type::VARARGS)
          @varargs = true
        else
          ps << t
        end
      end
      @param_types = ps
      @cif = 0
    end

    def result_type = @return_type

    # The prepared call interface, built once per signature.
    def cif
      @cif = FFI.build_cif(@return_type, @param_types, -1) if @cif == 0
      @cif
    end

    def write_native(addr, v, keep)
      a = if v.nil? then 0
          elsif v.is_a?(Function) then v.address
          elsif v.is_a?(Proc) || v.is_a?(Method)
            # one closure per callback type and proc: a proc passed again
            # (a comparator in a loop) reuses it
            key = "#{object_id}:#{v.object_id}"
            f = (FFI::CALLBACK_FUNCTIONS[key] ||= Function.new(self, v))
            keep << f if keep   # CALLBACK_FUNCTIONS holds it for good anyway
            f.address
          else FFI.pointer_address(v, keep)
          end
      Native.put_int(addr, K_PTR, a)
      nil
    end

    def read_native(addr)
      p = Native.get_int(addr, K_PTR)
      p == 0 ? nil : Function.new(self, Pointer.new(p))
    end
    def read_return(addr) = read_native(addr)
  end

  CallbackInfo = FunctionType
  FunctionInfo = FunctionType

  def self.build_cif(ret, params, nfixed)
    n = params.size
    at = Native.malloc(n * 8 + 8)
    i = 0
    while i < n
      Native.put_int(at + i * 8, Type::K_PTR, params[i].ffi_type)
      i += 1
    end
    cif = Native.cif_new(ret.ffi_type, n, at, nfixed)
    Native.free(at)     # cif_new keeps its own copy of the type list
    raise SignatureError, "libffi could not prepare the call signature" if cif == 0
    cif
  end

  # A variadic call's signature depends on the types passed at that call:
  # one cif per distinct signature, made once.
  VARIADIC_CIFS = {}
  def self.variadic_cif(ret, types, nfixed)
    key = "#{ret.object_id}:#{nfixed}:#{types.map(&:object_id).join(",")}"
    VARIADIC_CIFS[key] ||= build_cif(ret, types, nfixed)
  end

  # The closures made so far, by id: C calls come back through `dispatch`.
  # A closure lives as long as the program -- C may keep a callback past the
  # call that passed it -- so a proc passed as a callback gets one, reused.
  CLOSURES = []
  CALLBACK_FUNCTIONS = {}

  def self.dispatch(id, args, ret)
    f = CLOSURES[id]
    f.__dispatch_closure(args, ret)
    nil
  end

  NativeCallback.sp_ffi_set_dispatcher(FFI.method(:dispatch))

  # A Ruby proc registered under an attached function's name.
  class ProcFunction
    def initialize(prc)
      @prc = prc
    end

    def invoke(args, blk = nil)
      @prc.call(*args)
    end

    def call(*args) = @prc.call(*args)
  end

  class Function < Pointer
    attr_reader :function_type

    # Function.new(ret, params, proc_or_pointer = nil, options = {}) -- or,
    # given a FunctionType, Function.new(type, proc_or_pointer)
    def initialize(ret_or_type, params_or_target = nil, target = nil, options = nil, &blk)
      if ret_or_type.is_a?(FunctionType)
        @function_type = ret_or_type
        tgt = params_or_target
      else
        @function_type = FunctionType.new(ret_or_type, params_or_target || [], options)
        tgt = target
      end
      tgt = blk if tgt.nil? && blk
      @proc = nil
      @keep_result = []
      if tgt.is_a?(Proc) || tgt.is_a?(Method)
        @proc = tgt
        id = CLOSURES.size
        CLOSURES << self
        code = Native.closure_new(@function_type.cif, id)
        raise SignatureError, "could not create a closure" if code == 0
        super(code)
      else
        super(tgt.is_a?(AbstractMemory) ? tgt.address : (tgt || 0))
      end
    end

    def return_type = @function_type.return_type
    def param_types = @function_type.param_types
    def to_proc = proc { |*a| invoke(a, nil) }
    def autorelease = true
    def free = nil
    def inspect = "#<FFI::Function address=0x#{@address.to_s(16)}>"

    def call(*args, &blk) = invoke(args, blk)

    def invoke(args, blk)
      ft = @function_type
      params = ft.param_types
      if blk && params.last.is_a?(FunctionType)
        if args.size == params.size - 1
          args = args + [blk]
        elsif args.size == params.size && args.last.nil?
          args = args[0, args.size - 1] + [blk]
        end
      end
      raise NullPointerError, "invalid function pointer (NULL)" if @address == 0
      if ft.varargs
        raise ::ArgumentError, "wrong number of arguments (given #{args.size}, expected at least #{params.size})" if args.size < params.size
        extra = args.size - params.size
        raise ::ArgumentError, "varargs must come in type, value pairs" if extra.odd?
        types = params.dup
        vals = args[0, params.size]
        j = params.size
        while j < args.size
          t = FFI.find_type(args[j])
          # C's default argument promotions
          t = Type::FLOAT64 if t.equal?(Type::FLOAT32)
          t = Type::INT32 if t.integer? && t.size < 4
          types << t
          vals << args[j + 1]
          j += 2
        end
        cif = FFI.variadic_cif(ft.return_type, types, params.size)
        return FFI.__invoke(cif, @address, ft.return_type, types, vals)
      end
      if args.size != params.size
        raise ::ArgumentError, "wrong number of arguments (given #{args.size}, expected #{params.size})"
      end
      FFI.__invoke(ft.cif, @address, ft.return_type, params, args)
    end

    # A C call landed in this closure: unpack the arguments, run the proc,
    # store the result.
    def __dispatch_closure(args_addr, ret_addr)
      ft = @function_type
      params = ft.param_types
      vals = []
      i = 0
      while i < params.size
        va = Native.get_int(args_addr + i * 8, Type::K_PTR)
        vals << params[i].read_native(va)
        i += 1
      end
      r = @proc.call(*vals)
      rt = ft.return_type
      unless rt.void?
        rt.write_return(ret_addr, r, @keep_result)
        @keep_result.shift while @keep_result.size > 16
      end
      nil
    end
  end

  # One foreign call: marshal, call, unmarshal. Every argument gets a 16-byte
  # aligned slot (a struct by value gets its own size), and `avalues` points
  # at them.
  def self.__invoke(cif, fn, rtype, params, args)
    n = params.size
    total = 0
    offs = []
    i = 0
    while i < n
      offs << total
      sz = params[i].slot_size
      total += (sz + 15) / 16 * 16
      i += 1
    end
    rsize = rtype.slot_size < 16 ? 16 : rtype.slot_size
    buf_s = Native.zero_bytes(total + n * 8 + rsize + 16)
    base = Native.str_addr(buf_s)
    base = (base + 15) / 16 * 16
    av = base + total
    rv = av + n * 8
    keep = []
    i = 0
    while i < n
      slot = base + offs[i]
      params[i].write_native(slot, args[i], keep)
      Native.put_int(av + i * 8, Type::K_PTR, slot)
      i += 1
    end
    Native.call(cif, fn, av, rv)
    result = rtype.void? ? nil : rtype.read_return(rv)
    keep.clear
    buf_s = nil
    result
  end

  # ---- structs ----
  class StructLayout < Type
    class Field
      attr_reader :name, :offset, :type
      def initialize(name, offset, type)
        @name = name
        @offset = offset
        @type = type
      end
      def size = @type.size
      def alignment = @type.alignment
    end

    attr_reader :fields, :members

    def initialize(fields, size, alignment)
      super(K_STRUCT, size, alignment, "STRUCT")
      @fields = fields
      @members = fields.map(&:name)
      @by_name = {}
      fields.each { |f| @by_name[f.name] = f }
      @ffi_struct_type = 0
    end

    def [](name) = @by_name[name]
    def offsets = @fields.map { |f| [f.name, f.offset] }
    def offset_of(name) = @by_name[name].offset

    # libffi's view of the struct, for passing it by value: every field's
    # type in order (an inline array is its elements; a nested struct, its
    # own struct type).
    def ffi_type
      if @ffi_struct_type == 0
        elems = []
        @fields.each { |f| f.type.__ffi_elements(elems) }
        buf = Native.malloc(elems.size * 8 + 8)
        elems.each_with_index { |t, i| Native.put_int(buf + i * 8, K_PTR, t) }
        @ffi_struct_type = Native.type_struct(elems.size, buf)
      end
      @ffi_struct_type
    end
  end

  class Type
    def __ffi_elements(out)
      out << ffi_type
      out
    end
  end

  # A fixed-size array inside a struct: `[:int, 4]` or `[:char, 32]`.
  class ArrayType < Type
    attr_reader :elem_type, :length
    def initialize(elem, len)
      @elem_type = elem
      @length = len
      super(K_STRUCT, elem.size * len, elem.alignment, "ARRAY")
    end
    def __ffi_elements(out)
      i = 0
      while i < @length
        @elem_type.__ffi_elements(out)
        i += 1
      end
      out
    end
    def read_native(addr) = InlineArray.new(Pointer.new(addr).__with_size(size, @elem_type.size), self)
    def write_native(addr, v, keep)
      if v.is_a?(String)
        # the bytes themselves: only an array of chars takes them, as the gem
        unless @elem_type.integer? && @elem_type.size == 1
          raise ::NotImplementedError, "cannot set array field"
        end
        n = v.bytesize < size ? v.bytesize : size
        Native.memset(addr, 0, size)
        Native.write_bytes(addr, v, 0, n) if n > 0
      else
        i = 0
        while i < v.size && i < @length
          @elem_type.write_native(addr + i * @elem_type.size, v[i], keep)
          i += 1
        end
      end
      nil
    end
  end

  # A struct type used as a value: inline in another struct, or passed/returned
  # by value.
  class StructByValue < Type
    attr_reader :struct_class
    def initialize(klass)
      @struct_class = klass
      layout = klass.layout
      super(K_STRUCT, layout.size, layout.alignment, "STRUCT_BY_VALUE")
    end
    def layout = @struct_class.layout
    def ffi_type = @struct_class.layout.ffi_type
    def __ffi_elements(out)
      out << ffi_type
      out
    end
    def slot_size = size < 8 ? 8 : size
    def read_native(addr)
      # a copy: the source (a return slot, a callback argument) is transient
      s = @struct_class.new
      Native.memcpy(s.pointer.address, addr, size)
      s
    end
    def read_return(addr) = read_native(addr)
    def write_native(addr, v, keep)
      src = if v.is_a?(Struct) then v.pointer.address
            elsif v.is_a?(AbstractMemory) then v.address
            else raise ::ArgumentError, "expected a #{@struct_class} for a by-value struct, got #{v.class}"
            end
      Native.memcpy(addr, src, size)
      nil
    end
    def write_return(addr, v, keep) = write_native(addr, v, keep)
  end

  # A struct inline in another struct: reads give a view into the outer
  # struct's memory, not a copy.
  class InlineStruct < StructByValue
    def read_native(addr) = @struct_class.new(Pointer.new(addr).__with_size(size, 1))
  end

  class StructByReference < Type
    attr_reader :struct_class
    def initialize(klass)
      @struct_class = klass
      super(K_PTR, 8, 8, "STRUCT_BY_REFERENCE")
    end
    def native_type = Type::POINTER
    def to_native(v, ctx) = v.nil? ? Pointer::NULL : v.pointer
    def from_native(v, ctx) = @struct_class.new(v)
    def write_native(addr, v, keep)
      Native.put_int(addr, K_PTR, FFI.pointer_address(v, keep))
      nil
    end
    def read_native(addr) = @struct_class.new(Pointer.new(Native.get_int(addr, K_PTR)))
    def read_return(addr) = read_native(addr)
  end

  class Struct
    def self.layout(*spec)
      return @layout if spec.empty?
      @layout = __build_layout(spec, false)
    end

    def self.__build_layout(spec, union)
      pairs = []
      if spec.size == 1 && spec[0].is_a?(Hash)
        spec[0].each { |k, v| pairs << [k, v, nil] }
      else
        i = 0
        while i < spec.size
          name = spec[i]
          t = spec[i + 1]
          off = nil
          if i + 2 < spec.size && spec[i + 2].is_a?(Integer)
            off = spec[i + 2]
            i += 1
          end
          pairs << [name, t, off]
          i += 2
        end
      end
      fields = []
      offset = 0
      max_align = 1
      size = 0
      pairs.each do |name, t, off|
        type = __field_type(t)
        a = type.alignment
        a = 1 if a < 1
        a = @pack if @pack && a > @pack
        max_align = a if a > max_align
        if union
          fo = 0
        elsif off
          fo = off
        else
          fo = (offset + a - 1) / a * a
        end
        fields << StructLayout::Field.new(name, fo, type)
        offset = fo + type.size
        size = offset if offset > size
      end
      size = (size + max_align - 1) / max_align * max_align
      StructLayout.new(fields, size, max_align)
    end

    def self.__field_type(t)
      if t.is_a?(Array)
        ArrayType.new(__field_type(t[0]), t[1])
      elsif t.is_a?(Class) && t <= Struct
        InlineStruct.new(t)
      elsif t.is_a?(StructByValue)
        InlineStruct.new(t.struct_class)
      else
        FFI.find_type(t)
      end
    end

    def self.pack(n = nil)
      @pack = n if n
      @pack
    end

    def self.size = layout.size
    def self.alignment = layout.alignment
    def self.members = layout.members
    def self.offsets = layout.offsets
    def self.offset_of(name) = layout.offset_of(name)

    def self.by_value
      @by_value ||= StructByValue.new(self)
    end
    def self.val = by_value
    def self.by_ref(flags = nil)
      @by_ref ||= StructByReference.new(self)
    end
    def self.ptr(flags = nil) = by_ref
    def self.in = by_ref
    def self.out = by_ref

    def self.callback(params, ret) = FunctionType.new(ret, params)
    def self.enum(*args) = FFI.__make_enum(args, Enum)

    attr_reader :pointer

    def initialize(pointer = nil, *spec)
      if pointer.nil?
        @pointer = MemoryPointer.new(self.class.size)
      elsif pointer.is_a?(AbstractMemory)
        @pointer = pointer
      elsif pointer.is_a?(Integer)
        @pointer = Pointer.new(pointer)
      else
        if pointer.is_a?(String)
          raise ::TypeError, "wrong argument type String (expected FFI::AbstractMemory)"
        end
        @pointer = FFI::Pointer.new(FFI.pointer_address(pointer, nil))
      end
    end

    def layout = self.class.layout
    def to_ptr = @pointer
    def null? = @pointer.null?
    def size = self.class.size
    def members = self.class.members
    def offsets = self.class.offsets
    def offset_of(name) = self.class.offset_of(name)
    def order(*_) = self
    def clear
      Native.memset(@pointer.address, 0, size)
      self
    end

    def __field(name)
      f = self.class.layout[name]
      raise ::ArgumentError, "No such field '#{name}'" unless f
      f
    end

    def [](name)
      f = __field(name)
      FFI.__view_of(f.type.read_native(@pointer.address + f.offset), @pointer)
    end

    def []=(name, v)
      f = __field(name)
      a = @pointer.address + f.offset
      t = f.type
      t.write_native(a, v, nil)
      if t.is_a?(ArrayType) && v.is_a?(Array)
        # each element slot holds its own (and a shorter Array clears the rest)
        es = t.elem_type.size
        i = 0
        while i < t.length
          @pointer.__retain(a + i * es, i < v.size ? v[i] : nil)
          i += 1
        end
      else
        @pointer.__retain(a, v)
      end
      v
    end

    def values = members.map { |m| self[m] }
    def to_h
      h = {}
      members.each { |m| h[m] = self[m] }
      h
    end
    def inspect = "#<#{self.class.name} #{members.map { |m| "#{m}=#{self[m].inspect}" }.join(", ")}>"

  end

  # A fixed-size array field, viewed in place.
  class InlineArray
    include Enumerable
    def initialize(ptr, type)
      @pointer = ptr
      @type = type
    end
    def size = @type.length
    def length = @type.length
    def [](i)
      et = @type.elem_type
      FFI.__view_of(et.read_native(@pointer.address + i * et.size), @pointer)
    end
    def []=(i, v)
      et = @type.elem_type
      a = @pointer.address + i * et.size
      et.write_native(a, v, nil)
      @pointer.__retain(a, v)
    end
    def each
      i = 0
      while i < size
        yield self[i]
        i += 1
      end
      self
    end
    def to_a
      a = []
      i = 0
      while i < size
        a << self[i]
        i += 1
      end
      a
    end
    def to_ptr = @pointer
    def to_s
      @pointer.get_string(0, @type.size)
    end
  end

  class Union < Struct
    def self.layout(*spec)
      return @layout if spec.empty?
      @layout = __build_layout(spec, true)
    end
  end

  def self.__make_enum(args, klass)
    native = nil
    args = args.dup
    native = args.shift if args[0].is_a?(Type) && !args[0].is_a?(Enum)
    if args.size == 2 && args[0].is_a?(Symbol) && args[1].is_a?(Array)
      klass.new(args[1], args[0], native)
    elsif args.size == 1 && args[0].is_a?(Array)
      klass.new(args[0], nil, native)
    else
      klass.new(args, nil, native)
    end
  end

  # ---- libraries ----
  class DynamicLibrary
    RTLD_LAZY = Native.rtld(0)
    RTLD_NOW = Native.rtld(1)
    RTLD_GLOBAL = Native.rtld(2)
    RTLD_LOCAL = Native.rtld(3)

    attr_reader :name, :handle

    def initialize(name, handle)
      @name = name
      @handle = handle
    end

    def self.open(name, flags = RTLD_LAZY | RTLD_LOCAL)
      h = Native.dlopen(name || "", flags)
      raise LoadError, "Could not open library '#{name}': #{Native.dlerror}" if h == 0
      DynamicLibrary.new(name, h)
    end

    def self.try_open(name, flags)
      h = Native.dlopen(name || "", flags)
      h == 0 ? nil : DynamicLibrary.new(name, h)
    end

    def find_function(sym)
      a = Native.dlsym(@handle, sym)
      a == 0 ? nil : Pointer.new(a)
    end
    def find_symbol(sym) = find_function(sym)
    def find_variable(sym) = find_function(sym)
    def last_error = Native.dlerror
  end

  CURRENT_PROCESS = "__current_process__"
  USE_THIS_PROCESS_AS_LIBRARY = CURRENT_PROCESS

  def self.map_library_name(lib)
    return lib if lib.include?("/")
    n = lib
    n = Platform::LIBPREFIX + n unless n.start_with?(Platform::LIBPREFIX)
    n += ".#{Platform::LIBSUFFIX}" unless n.include?(".#{Platform::LIBSUFFIX}")
    n
  end

  LIB_SEARCH_DIRS = ["/usr/lib/", "/usr/local/lib/", "/opt/local/lib/", "/opt/homebrew/lib/"]

  def self.__open_library(name, flags)
    return DynamicLibrary.new(CURRENT_PROCESS, Native.dlopen("", flags)) if name == CURRENT_PROCESS
    candidates = []
    names = name.is_a?(Array) ? name : [name]
    names.each do |n|
      s = n.to_s
      candidates << s unless candidates.include?(s)
      m = map_library_name(s)
      candidates << m unless candidates.include?(m)
    end
    errors = []
    candidates.each do |c|
      lib = DynamicLibrary.try_open(c, flags)
      return lib if lib
      errors << Native.dlerror
      unless c.start_with?("/")
        LIB_SEARCH_DIRS.each do |dir|
          path = dir + c
          next unless File.exist?(path)
          lib = DynamicLibrary.try_open(path, flags)
          return lib if lib
          errors << Native.dlerror
        end
      end
    end
    shown = names.map(&:to_s)
    raise LoadError, "Could not open library '#{shown.size == 1 ? shown[0] : shown.inspect}': #{errors.join(".\n")}"
  end

  module Library
    CURRENT_PROCESS = FFI::CURRENT_PROCESS
    LIBC = FFI::Platform::LIBC
    FLAGS = { global: DynamicLibrary::RTLD_GLOBAL, local: DynamicLibrary::RTLD_LOCAL,
              lazy: DynamicLibrary::RTLD_LAZY, now: DynamicLibrary::RTLD_NOW }

    def ffi_lib(*names)
      raise LoadError, "library names list must not be empty" if names.empty?
      flags = @ffi_lib_flags || (DynamicLibrary::RTLD_LAZY | DynamicLibrary::RTLD_LOCAL)
      # each argument is one library; an Array argument lists alternatives
      # for it, the first that opens wins
      libs = []
      names.each { |n| libs << FFI.__open_library(n, flags) }
      @ffi_libs = libs
    end

    def ffi_libraries
      raise LoadError, "no library specified" if @ffi_libs.nil?
      @ffi_libs
    end

    def ffi_lib_flags(*flags)
      f = 0
      flags.each { |x| f |= FLAGS[x] || 0 }
      @ffi_lib_flags = f
    end

    def ffi_convention(c = nil)
      @ffi_convention = c if c
      @ffi_convention || :default
    end

    def find_type(t)
      if t.is_a?(Symbol)
        m = @ffi_typedefs
        return m[t] if m && m.key?(t)
        e = @ffi_enums
        if e
          en = e.find(t)
          return en if en
        end
      end
      FFI.find_type(t)
    end

    def typedef(old, add, info = nil)
      @ffi_typedefs ||= {}
      t = if old.is_a?(Symbol) && old == :enum
            en = FFI.__make_enum(info.is_a?(Array) ? [add, info] : [add], Enum)
            (@ffi_enums ||= Enums.new) << en
            en
          else
            find_type(old)
          end
      @ffi_typedefs[add] = t
      FFI::TYPEDEFS[add] = t
      t
    end

    def callback(*args)
      if args.size == 3
        name = args[0]
        ft = FunctionType.new(find_type(args[2]), args[1].map { |x| find_type(x) })
        typedef(ft, name)
        ft
      else
        FunctionType.new(find_type(args[1]), args[0].map { |x| find_type(x) })
      end
    end

    def enum(*args) = __ffi_generic_enum(args, false)
    def bitmask(*args) = __ffi_generic_enum(args, true)

    def __ffi_generic_enum(args, bitmask)
      a = args.dup
      native = nil
      native = find_type(a.shift) if a[0].is_a?(Type) || (a[0].is_a?(Symbol) && a.size > 1 && a[1].is_a?(Symbol) && a.size == 3 && a[2].is_a?(Array))
      name = nil
      info = nil
      if a.size == 2 && a[0].is_a?(Symbol) && a[1].is_a?(Array)
        name = a[0]
        info = a[1]
      elsif a.size == 1 && a[0].is_a?(Array)
        info = a[0]
      else
        info = a
      end
      en = bitmask ? Bitmask.new(info, name, native) : Enum.new(info, name, native)
      (@ffi_enums ||= Enums.new) << en
      if name
        @ffi_typedefs ||= {}
        @ffi_typedefs[name] = en
        FFI::TYPEDEFS[name] = en
      end
      en
    end

    def enum_type(name) = @ffi_enums ? @ffi_enums.find(name) : nil
    def enum_value(sym) = @ffi_enums ? @ffi_enums.__map_symbol(sym) : nil

    # attach_function(name, [c_name,] params, ret, options = {})
    def attach_function(name, a1, a2 = nil, a3 = nil, a4 = nil)
      if a1.is_a?(Array)
        cname = name
        params = a1
        ret = a2
        opts = a3
      else
        cname = a1
        params = a2
        ret = a3
        opts = a4
      end
      ptypes = params.map { |t| find_type(t) }
      rtype = find_type(ret)
      addr = 0
      libs = @ffi_libs || [FFI.__open_library(CURRENT_PROCESS, DynamicLibrary::RTLD_LAZY)]
      csym = cname.to_s
      libs.each do |lib|
        a = Native.dlsym(lib.handle, csym)
        if a != 0
          addr = a
          break
        end
      end
      if addr == 0
        raise FFI::NotFoundError, "Function '#{csym}' not found in [#{libs.map(&:name).join(", ")}]"
      end
      fn = Function.new(FunctionType.new(rtype, ptypes), Pointer.new(addr))
      @ffi_functions ||= {}
      @ffi_functions[name.to_sym] = fn
      ::FFI__Registry.__add(self, name.to_sym, fn)
      fn
    end

    def __ffi_call(name, args, blk = nil, &b)
      fns = @ffi_functions
      fn = fns ? fns[name] : nil
      raise NoMethodError, "undefined method '#{name}' for #{self}" unless fn
      fn.invoke(args, blk || b)
    end

    # `define_singleton_method name, -> { fallback }` standing in for a
    # function the library lacks (the expansion of a class-body macro that
    # rescues attach_function's NotFoundError): calls of the attached name
    # reach the proc.
    def __ffi_define_proc(name, prc)
      @ffi_functions ||= {}
      @ffi_functions[name.to_sym] = ProcFunction.new(prc)
      ::FFI__Registry.__add(self, name.to_sym, @ffi_functions[name.to_sym])
      name.to_sym
    end

    def __ffi_attached?(name)
      fns = @ffi_functions
      fns ? fns.key?(name.to_sym) : false
    end

    def attached_functions = @ffi_functions || {}
    def function_names(name, arg_types) = [name.to_s]

    # attach_variable(name, [c_name,] type)
    def attach_variable(name, a1, a2 = nil)
      cname = a2 ? a1 : name
      t = find_type(a2 || a1)
      addr = 0
      (@ffi_libs || []).each do |lib|
        a = Native.dlsym(lib.handle, cname.to_s)
        if a != 0
          addr = a
          break
        end
      end
      raise FFI::NotFoundError, "Unable to find variable '#{cname}'" if addr == 0
      @ffi_variables ||= {}
      @ffi_variables[name.to_sym] = [addr, t]
      nil
    end

    def __ffi_var_get(name)
      addr, t = @ffi_variables[name]
      t.read_native(addr)
    end

    def __ffi_var_set(name, v)
      addr, t = @ffi_variables[name]
      t.write_native(addr, v, nil)
      # the C global now points into it: held for as long as it does
      @ffi_variable_refs ||= {}
      if v.is_a?(AbstractMemory) || v.is_a?(Struct)
        @ffi_variable_refs[name] = v
      else
        @ffi_variable_refs.delete(name)
      end
      v
    end
  end

  # A pointer that frees itself: the releaser runs when the object is
  # collected (or at the end of the program, like any finalizer) unless
  # autorelease was turned off, or when #free is called -- once either way.
  class AutoPointer < Pointer
    # The finalizer's state, apart from the pointer: a finalizer that
    # reached the pointer would keep it alive.
    class Release
      attr_accessor :auto
      def initialize(address, releaser)
        @address = address
        @releaser = releaser
        @auto = true
      end
      def run
        r = @releaser
        @releaser = nil
        r.call(Pointer.new(@address)) if r && @address != 0
        nil
      end
      def collected
        run if @auto
        nil
      end
    end

    def self.__finalizer(rel) = proc { |_id| rel.collected }

    def initialize(ptr, releaser = nil)
      super(ptr.is_a?(AbstractMemory) ? ptr.address : ptr)
      @release = Release.new(@address, releaser)
      ObjectSpace.define_finalizer(self, AutoPointer.__finalizer(@release))
    end
    def free
      @release.run
      @release.auto = false
      @address = 0
      nil
    end
    def autorelease=(v)
      @release.auto = v
    end
    def autorelease? = @release.auto
  end

  class ManagedStruct < Struct
  end

  def self.__sizeof_pointer = Native.sizeof(0)
end

# Every function any FFI::Library module attached, by the module's name and
# the function's: the target of a bare call the compiler could not bind to a
# definition (a function attached under a name computed at run time, called
# from code that includes the module). The call names the library modules in
# reach, nearest ancestor first, and the first that attached the name answers
# -- a same-named function of an unrelated library is never taken. See
# rewrite_ffi_dynamic_calls in the compiler.
module FFI__Registry
  FUNCTIONS = {}

  def self.__add(mod, name, fn)
    (FUNCTIONS[mod.name.to_s.to_sym] ||= {})[name] = fn
  end

  def self.__ffi_dispatch(owners, name, args, blk = nil, &b)
    owners.each do |o|
      fns = FUNCTIONS[o]
      fn = fns ? fns[name] : nil
      return fn.invoke(args, blk || b) if fn
    end
    raise NoMethodError, "undefined method '#{name}' for main"
  end
end
