# the read and the shadowing def run in a loop at the top level
module M; def v = 1; end
class C; extend M; end
2.times do
  puts C.v
  def C.v = 2
end
