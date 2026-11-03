# A constant path whose scope is a builtin class or module and whose last
# name is another top-level builtin (Array::String) raises NameError, as in
# CRuby: a scoped lookup does not fall back to Object. Real nested
# constants (Thread::Mutex, Float::INFINITY) still resolve.
x = (Array::String rescue "NE")
p x
p((Array::String.new("x") rescue "NE"))
p((Array::Hash.new rescue "NE"))
p((Comparable::Array.new(2) rescue "NE"))
p((Kernel::String rescue "NE"))
p((StandardError::ArgumentError rescue "NE"))
p defined?(Array::String), defined?(Comparable::Integer)
p Object::String, Thread::Mutex, Process::Status, Errno::ENOENT, Float::INFINITY, Encoding::UTF_8
p BasicObject::BasicObject
def k(c) = c.name
p((k(Array::Integer) rescue "NE"))
begin
  Array::String
rescue NameError => e
  p e.class, e.message
end
module Wrap; end
p((Wrap::Array::String rescue "NE"))
p String, Array
