# As comparable_copy_alias_kernel_name.rb, with the alias in a module N
# that M prepends, and M included at the top level: CRuby prints N1,
# the copy called Kernel's p. Refused.
module N
  def original(x) = "N#{x}"
  alias p original
end
module M
  prepend N
end
include M
module Comparable
  def qa = p(1)
end
puts "x".qa
