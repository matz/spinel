# The `**` operand's to_hash runs after the receiver and the positional operands and before the `&` operand.
# order: receiver, positional, the ** operand and its to_hash, then the &block operand and its to_proc
$l = []
class H; def to_hash = ($l << :to_hash; {}); end
class P; def to_proc = ($l << :to_proc; proc { |x| x }); end
def h = ($l << :h; H.new)
def b = ($l << :b; P.new)
def r = ($l << :r; [3, 1, 2])
def a = ($l << :a; 1)
x = r.push(a, **h, &b); p x, $l
y = (r.each_slice(a, **h, &b).to_a rescue $!.class); p y, $l
