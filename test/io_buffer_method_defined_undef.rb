# IO::Buffer.method_defined? is false for a bound method the program undefines.

class IO::Buffer
  undef_method :size
end
p IO::Buffer.method_defined?(:size)
p IO::Buffer.method_defined?(:xor!)
