# A method added to `module Comparable` may call Kernel's p, format or
# puts with no receiver when the program has no method of that name on
# String, Symbol, Object or the top level. A p in an unrelated class
# does not change that, also when an alias or alias_method makes it.
class Other
  def p(x) = "other"
end
class Another
  def original(x) = "another"
  alias p original
  alias_method :format, :original
end
module Unrelated
  def original(x) = "unrelated"
  alias p original
end
class Other
  include Unrelated
end
module Comparable
  def qa = format("%d", 3)
  def qb = (puts "hi"; 7)
  def qc = p(5)
end
puts "x".qa
p :s.qb
puts "y".qc
