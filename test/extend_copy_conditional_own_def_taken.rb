# the shadowing def runs under a condition that holds
module M; def v = 1; end
class C; extend M; end
puts C.v
if ARGV.empty?
  def C.v = 2
end
puts C.v
