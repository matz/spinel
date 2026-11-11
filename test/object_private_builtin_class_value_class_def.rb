# A private method of a reopened Object, called on a builtin class: a
# method the reopened Class defines itself is public and answers. A module
# value is not a Class and still meets Object's private method.
class Object; private def top = "t"; end
class Class; define_method(:top) { "dm" }; end
def t
  yield
rescue NoMethodError => e
  puts "NME #{e.message}"
end
t { puts Integer.top }
k = String
t { puts k.top }
t { puts Kernel.top }
