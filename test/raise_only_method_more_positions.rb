# A call on a value that never comes back -- a method whose every path raises,
# typed void -- never runs: Ruby evaluates the receiver first, and it raises.
# Each operator's arm typed the receiver from its void and refused the
# program at compile time: arithmetic, a comparison, a method called on it,
# and every call further along a chain of them.

class Migration
  def version = raise(NotImplementedError, "subclass")
end

def attempt(label)
  yield
rescue NotImplementedError => e
  puts "#{label}: #{e.message}"
end

m = Migration.new
attempt("arithmetic") { p(m.version + "x") }
attempt("comparison") { p(m.version < 3) }
attempt("method call") { p m.version.size }
attempt("chain") { p(m.version - 1 > 2) }
attempt("chain interpolated") { puts "n=#{m.version.size}" }
attempt("local") { x = m.version * 2; p x }

# through a boxed receiver, as a framework's loop over its migrations
ms = [Migration.new]
attempt("boxed") { p ms.map { |mm| mm.version.size + 1 } }

# the argument of a call that never runs is not evaluated
def noisy
  puts "argument evaluated"
  1
end
attempt("argument") { p(m.version + noisy) }
