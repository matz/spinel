# IO::Buffer.method_defined? is true for the methods the runtime binds
# (xor!, get_value, size), as respond_to? on an IO::Buffer is.

p IO::Buffer.method_defined?(:xor!)
p IO::Buffer.method_defined?(:get_value)
p IO::Buffer.method_defined?(:size)
p IO::Buffer.method_defined?("size")
p IO::Buffer.method_defined?(:size, false)
p IO::Buffer.public_method_defined?(:xor!)
p IO::Buffer.private_method_defined?(:xor!)
p IO::Buffer.protected_method_defined?(:xor!)
p IO::Buffer.method_defined?(:each)
p IO::Buffer.method_defined?(:nope)
p IO::Buffer.method_defined?(:object_id)
p IO::Buffer.method_defined?(:object_id, false)

# protocol-websocket's choice of its masking code
if IO.const_defined?(:Buffer) && IO::Buffer.respond_to?(:for) && IO::Buffer.method_defined?(:xor!)
  def mask_xor(data, mask)
    buffer = data.dup
    mask_buffer = IO::Buffer.for(mask)
    IO::Buffer.for(buffer) { |b| b.xor!(mask_buffer) }
    buffer
  end
  puts "IO::Buffer"
else
  def mask_xor(data, mask)
    data.bytes.each_with_index.map { |b, i| b ^ mask.getbyte(i % 4) }.pack("C*")
  end
  puts "per-byte"
end
p mask_xor("hello".b, "abcd".b).bytes
