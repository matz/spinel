# a bare call in the class's own class method runs before and after the shadowing def
module M; def v = 1; end
class C
  extend M
  def self.show = v
end
puts C.show
def C.v = 2
puts C.show
