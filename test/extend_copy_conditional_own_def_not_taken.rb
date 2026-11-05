# the shadowing def sits under a condition that does not hold: the copy answers throughout
module M; def v = 1; end
class C; extend M; end
puts C.v
unless ARGV.empty?
  def C.v = 2
end
puts C.v
