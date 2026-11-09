# A private method of a reopened Object, called with a builtin class or
# module as the receiver, raises NoMethodError, unless a module that a
# reopened Class or Module mixes in gives the class or module a public one.
class Object; private def top = "t"; def pub = top; end
module Mx; def top = "mx"; end
module My; include Mx; end
class Class; include My; end
def t
  yield
rescue NoMethodError => e
  puts "NME #{e.message}"
end
t { puts Integer.top }
t { puts String.top }
t { puts Kernel.top }
t { puts Comparable.top }
t { puts Integer.pub }
t { puts Integer.send(:top) }
t { puts 3.top }
