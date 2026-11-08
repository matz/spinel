# FFI::Pointer#read_bytes(0) and get_bytes(off, 0) answer a new, mutable
# binary String, as every other length does (#11341)
require "ffi"
ptr = FFI::MemoryPointer.new(:char, 4)
s = ptr.read_bytes(0)
p s.frozen?
p s.encoding
s.force_encoding(Encoding::UTF_8)
s << "x"
p s
t = ptr.get_bytes(1, 0)
t << "y"
p t
p ptr.read_bytes(0).equal?(ptr.read_bytes(0))
