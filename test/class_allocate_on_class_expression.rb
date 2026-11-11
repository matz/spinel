# `allocate` on a class value that is not a written-out constant: the class
# an expression answers (`x.class`). It raised no TypeError and built no
# instance: the compiler refused the program ("unsupported call"), for every
# class. It now reaches the class allocation dispatch a boxed class value
# does: a class with no allocator raises TypeError naming it, and the others
# answer an instance that did not run initialize.
class Pt
  def initialize(x); puts "initialize #{x}"; @x = x; end
  attr_reader :x
end
class Sub < Pt; end

def try(label)
  r = yield
  puts "#{label}: #{r.class}"
rescue TypeError => e
  puts "#{label}: TypeError #{e.message}"
rescue NoMethodError
  puts "#{label}: NoMethodError"
end

# classes with no allocator
try("Float") { 1.0.class.allocate }
try("Integer") { 1.class.allocate }
try("nil") { nil.class.allocate }
try("true") { true.class.allocate }
try("false") { false.class.allocate }
try("Symbol") { :a.class.allocate }
x = 2.5
try("local Float") { x.class.allocate }
y = [1, 2.5].first
try("Array element") { y.class.allocate }

# classes that allocate
try("String") { "s".class.allocate }
try("Array") { [].class.allocate }
try("Hash") { {}.class.allocate }
try("Object") { Object.new.class.allocate }

# a program's own class: an instance, initialize not run
pt = Pt.new(1).class.allocate
puts pt.class
p pt.x
sub = Sub.new(2).class.allocate
puts sub.class
p sub.instance_of?(Sub)

# the class of the value in a variable
obj = Pt.new(3)
k = obj.class
puts k.allocate.class
puts obj.class.allocate.class
