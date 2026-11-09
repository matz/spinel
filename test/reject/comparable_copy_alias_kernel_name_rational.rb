# As comparable_copy_alias_kernel_name.rb, with the alias in Numeric,
# which a Rational reaches before Comparable: CRuby prints N1, the copy
# called Kernel's p. Refused.
class Numeric
  def original(x) = "N#{x}"
  alias p original
end
module Comparable
  def qa = p(1)
end
puts Rational(1, 2).qa
