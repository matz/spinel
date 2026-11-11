# As comparable_copy_alias_kernel_name_rational.rb, with an alias_method
# in Rational itself: CRuby prints R2, the copy called Kernel's format.
# Refused.
class Rational
  def original(*x) = "R#{x.size}"
  alias_method :format, :original
end
module Comparable
  def qa = format("%d", 1)
end
puts Rational(1, 2).qa
