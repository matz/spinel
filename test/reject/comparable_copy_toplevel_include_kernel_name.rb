# As comparable_copy_own_kernel_name.rb, with p from a module the top
# level includes: CRuby prints M1, the copy called Kernel's p. Refused.
module M
  def p(x) = "M#{x}"
end
include M
module Comparable
  def qa = p(1)
end
puts "x".qa
