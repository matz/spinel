# A POLY String parameter handed on through a chain of calls deeper than
# the old fixed bound: one that never appends goes over as it is, one that
# appends at the end of the chain shares the caller's String.
def l3(name) = name.size
def l2(name) = l3(name)
def l1(name) = l2(name)
def set(name, v) = l1(name)
h = {arg_name: "in", flags: 3}
p set(h[:arg_name], 1)
an = :out.to_s
p set(an, 2)

def m6(s) = (s << "!"; 0)
def m5(s) = m6(s)
def m4(s) = m5(s)
def m3(s) = m4(s)
def m2(s) = m3(s)
def m1(s, v) = m2(s)
m1([1, +"z"][1], 1)
t = +"y"
m1(t, 2)
p t

# mutual recursion that never appends
def r1(s, n) = n > 0 ? r2(s, n - 1) : s.size
def r2(s, n) = r1(s, n)
p r1(h[:arg_name], 3)
u = :zz.to_s
p r1(u, 3)
p u

# a cycle with an append off it
def q1(s, n) = n > 0 ? q2(s, n - 1) : 0
def q2(s, n) = (n == 1 ? q3(s) : q1(s, n))
def q3(s) = (s << "?"; 0)
q1([1, +"a"][1], 3)
w = +"w"
q1(w, 3)
p w
