# A respond_to_missing? in a Float reopening, a module an Integer
# reopening includes and an alias in a Numeric reopening still answer
# respond_to? for a name CRuby's class lacks.
class Float
  def respond_to_missing?(n, priv = false) = n == :to_a || super
end
module Conv
  def to_h = {}
end
class Integer; include Conv; end
p 7.5.respond_to?(:to_a), 7.respond_to?(:to_h), 7.respond_to?(:to_a)
class Numeric
  alias to_str inspect
end
module Walk
  include Comparable
end
class Float
  include Walk
end
p 7.respond_to?(:to_str), 7.5.respond_to?(:between?)
