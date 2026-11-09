# A builtin class with no allocator answers `allocate` with a TypeError naming
# it, as CRuby does, whether the class is written out or reached through a
# variable. Integer was the one class that did; NilClass, TrueClass,
# FalseClass, Float, Symbol, Proc, Thread, Struct and Data raised
# NoMethodError instead. The classes whose allocate CRuby answers, and the two
# whose allocate is undefined (Rational, Complex), keep their answers.
def try(label)
  r = yield
  puts "#{label}: #{r.class}"
rescue TypeError => e
  puts "#{label}: TypeError #{e.message}"
rescue NoMethodError
  puts "#{label}: NoMethodError"
end

try("NilClass") { NilClass.allocate }
try("TrueClass") { TrueClass.allocate }
try("FalseClass") { FalseClass.allocate }
try("Integer") { Integer.allocate }
try("Float") { Float.allocate }
try("Symbol") { Symbol.allocate }
try("Proc") { Proc.allocate }
try("Thread") { Thread.allocate }
try("Struct") { Struct.allocate }
try("Data") { Data.allocate }

# the same through a variable, which takes the runtime's dispatch
[NilClass, TrueClass, FalseClass, Integer, Float, Symbol, Proc, Thread, Struct, Data].each do |k|
  try("via #{k}") { k.allocate }
end

# unchanged
try("Rational") { Rational.allocate }
try("Complex") { Complex.allocate }
try("String") { String.allocate }
try("Array") { Array.allocate }
try("Hash") { Hash.allocate }
try("Object") { Object.allocate }
class Plain; end
try("Plain") { Plain.allocate }
