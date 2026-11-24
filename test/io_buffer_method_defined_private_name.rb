# A private :size on IO::Buffer makes IO::Buffer.method_defined?(:size) and
# public_method_defined?(:size) false.

class IO::Buffer
  private :size
end
p IO::Buffer.public_method_defined?(:size)
p IO::Buffer.private_method_defined?(:size)
p IO::Buffer.method_defined?(:size)
p IO::Buffer.method_defined?(:xor!)
m = ARGV.empty? ? :size : :nope
p IO::Buffer.public_method_defined?(m)
p IO::Buffer.private_method_defined?(m)
