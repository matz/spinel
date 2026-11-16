# A String, a Symbol, an Integer or a Float compared with an Array is never
# equal: == answers false and != true, and both operands still run.
s = "r"
a = ["r"]
p(s == a)
p(s != a)
p(1 == [1], 1 != [1])
p(:a == [:a], :a != [:a])
p(1.5 == [1.5])
n = [1, 2]
def side(log) = (log << :ran; "x")
log = []
p(side(log) == n, log)
p(a == s, a != s)
x = 3
p(x == n, n == x)
