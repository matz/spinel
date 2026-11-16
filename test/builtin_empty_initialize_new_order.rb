# The operands of a builtin `new` whose empty initialize only receives them
# run in the order a call builds its argument list: each argument as written,
# a splat's to_a where it stands, the block pass last, and its to_proc must
# answer a Proc.
class Array
  def initialize(*, &); end
end
$log = []
def a1 = ($log << :a1; 1)
def a2 = ($log << :a2; 2)
def pr = ($log << :pr; proc { })
class S
  def to_a = ($log << :to_a; [])
end
s = S.new
p Array.new(a1, *s, a2, &pr)
p $log
class P
  def to_proc = "not a proc"
end
begin
  Array.new(&P.new)
rescue TypeError => e
  p e.class
end
