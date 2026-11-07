# const_defined? knows the builtin classes and modules CRuby defines at
# boot, by a literal name and by a name known only at run time.
p Object.const_defined?(:Struct)
p Object.const_defined?(:Thread)
p Object.const_defined?(:StandardError)
p Object.const_defined?(:Errno)
p Object.const_defined?(:Encoding)
p Object.const_defined?(:Mutex)
p Object.const_defined?(:SocketError)
p Object.const_defined?(:TCPSocket)
p Object.const_defined?(:NoSuchThing)
module M; W = 1; end
class K; end
p M.const_defined?(:Struct)
p K.const_defined?(:ZeroDivisionError)
p K.const_defined?(:Struct, false)
names = %w[Struct StandardError Thread Errno Encoding Process KeyError Data Fiber NoSuchThing]
names.each { |n| p [n, Object.const_defined?(n)] }
p M.const_defined?(["Thread"].join)
p M.const_defined?(["Struct"].join, false)
k = [K, M][0]
p k.const_defined?(:Struct)
