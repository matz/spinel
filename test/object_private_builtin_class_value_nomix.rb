# A private method of a reopened Object, called with a builtin class or
# module as the receiver, raises NoMethodError.
class Object; private def top = "t"; end
def t
  yield
rescue NoMethodError => e
  puts "NME #{e.message}"
end
t { puts Integer.top }
t { puts Kernel.top }
