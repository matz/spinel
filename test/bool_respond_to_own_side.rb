# respond_to? on true or false where only TrueClass or FalseClass defines
# the name: each value answers by its own class
class TrueClass;  def e = 1; end
class FalseClass; def f = 2; end

p true.respond_to?(:e), false.respond_to?(:e)
p true.respond_to?(:f), false.respond_to?(:f)
p true.respond_to?("e"), false.respond_to?("e")

# a value only the run time knows
def r(b) = b.respond_to?(:e)
p r(true), r(false)
p [true, false].map { |v| v.respond_to?(:e) }
v = (1 > 2)
p v.respond_to?(:e), (!v).respond_to?(:e)
x = [true, false][ARGV.size]
y = [false, true][ARGV.size]
p x.respond_to?(:e), y.respond_to?(:e), x.respond_to?(:f), y.respond_to?(:f)

# as a condition
if false.respond_to?(:e) then p :bad else p :good end
def g(b) = b.respond_to?(:e) ? :yes : :no
p g(true), g(false)

# the builtin surface stays
p false.respond_to?(:to_s), true.respond_to?(:&)

# a private method answers only to include_all, a literal or a run-time one
class TrueClass; private def pe = 1; end
p true.respond_to?(:pe), true.respond_to?(:pe, true), false.respond_to?(:pe, true)
def k(b, ia) = b.respond_to?(:pe, ia)
p k(true, false), k(true, true), k(false, true)

# a method an included module gives FalseClass
module M; def mm = 1; end
class FalseClass; include M; end
p false.respond_to?(:mm), true.respond_to?(:mm)

# a top-level def is Object's private method
def e2 = 0
class TrueClass; def e2 = 1; end
p false.respond_to?(:e2), false.respond_to?(:e2, true), true.respond_to?(:e2, true)

# the answer boxed
z = [1, "s", false.respond_to?(:e), true.respond_to?(:e)]
p z

# an alias of TrueClass's own method
class TrueClass; alias e6 e; alias_method :e7, :e; end
p true.respond_to?(:e6), false.respond_to?(:e6), r(false), false.respond_to?(:e7)
