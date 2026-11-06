# A `class << D` in a method body defines D's class method k each time the
# method runs, and not before: CRuby prints "false", "nil", then "true".
# The compiled method table would hold k from the start.
D = Data.define(:v)
def setup
  class << D
    def k = :mk
  end
end
p D.respond_to?(:k), defined?(D.k)
setup
p D.respond_to?(:k)
