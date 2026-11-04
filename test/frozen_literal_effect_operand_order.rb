# A frozen String literal that statements run before, `(f; "lit")`, is an
# argument whose effect is ordered with the arguments after it: Ruby runs
# f before the later argument's call, whatever order C evaluates a call's
# arguments in.
$log = []
def eff(t) = ($log << t; t.to_s * 2)
p "abc".sub((eff(:r); "b"), eff(:s)), $log
$log = []
p "abc".tr((eff(:t); "a"), (eff(:u); "z") + eff(:w)), $log
$log = []
a = []
a.push((eff(:a); "lit"), eff(:b))
p a, $log
$log = []
x = [(eff(:p); "lit"), eff(:q)]
p x, $log
$log = []
h = {}
h.store((eff(:k); "key"), eff(:v))
p h, $log
# a raise is an effect too: 1 / z raises before the later argument runs
$log = []
z = 0
begin
  "abc".sub((1 / z; "b"), eff(:s))
rescue ZeroDivisionError => e
  p e.class, $log
end
