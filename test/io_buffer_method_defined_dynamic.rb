# IO::Buffer.method_defined? with the name or the class held in a variable
# is true for the methods the runtime binds, same as with literals.

m = ARGV.empty? ? :xor! : :nope
p IO::Buffer.method_defined?(m)
p IO::Buffer.public_method_defined?(m)
p IO::Buffer.private_method_defined?(m)
n = ARGV.empty? ? :nope : :xor!
p IO::Buffer.method_defined?(n)
k = ARGV.empty? ? IO::Buffer : String
p k.method_defined?(:xor!)
p k.method_defined?(:upcase)
