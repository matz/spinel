# the shadowing def is made in `class << C`
module M; def v = 1; end
class C; extend M; end
puts C.v
class << C
  def v = 2
end
puts C.v
