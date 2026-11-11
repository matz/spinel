# As comparable_copy_own_kernel_name.rb, with p from an alias in a module
# that String includes: CRuby prints M1, the copy called Kernel's p.
# Refused. An alias in a class or module no String reaches is fine
# (comparable_copy_kernel_name.rb).
module M
  def original(x) = "M#{x}"
  alias p original
end
class String
  include M
end
module Comparable
  def qa = p(1)
end
puts "x".qa
