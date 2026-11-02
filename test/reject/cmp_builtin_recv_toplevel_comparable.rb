# `>=` on a value typed as an Exception, or `>` on true, raises NoMethodError
# when the class has no such operator (test/cmp_on_builtin_without_operator.rb).
# Here the top-level `include Comparable` gives Object, and so every such
# value, Comparable's operators over the program's `<=>`: CRuby prints true
# twice. Refused, not answered with NoMethodError; with no `<=>` CRuby would
# raise ArgumentError, which NoMethodError is not either.
include Comparable
def <=>(other) = 1

def ge(x) = x >= 0
begin
  p ge(RuntimeError.new("x"))
rescue ArgumentError => e
  puts e.message
end
p((1 == 1) > 0)
