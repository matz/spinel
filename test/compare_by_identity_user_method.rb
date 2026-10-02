# A user class's own compare_by_identity is the program's method, not
# Hash#compare_by_identity: the Hash refusal (#2086) fired on the name alone
# and rejected a program CRuby runs. Called on an instance, through an
# inherited definition, and as an implicit-self call.
class Registry
  def initialize; @mode = :value; end
  def compare_by_identity
    @mode = :identity
    self
  end
  def mode = @mode
end
r = Registry.new
p r.mode
p r.compare_by_identity.mode
p Registry.new.compare_by_identity.mode

class Base
  def compare_by_identity = "base"
end
class Kid < Base
  def go = compare_by_identity
end
p Kid.new.compare_by_identity
p Kid.new.go
