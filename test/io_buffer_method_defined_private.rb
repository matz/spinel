# A private def over an IO::Buffer binding makes IO::Buffer.method_defined?
# and public_method_defined? false.

class IO::Buffer
  private def size = 0
end
b = IO::Buffer.new(4)
p IO::Buffer.public_method_defined?(:size)
p IO::Buffer.private_method_defined?(:size)
p IO::Buffer.method_defined?(:size)
p b.respond_to?(:xor!)
