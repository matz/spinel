# `.to_s` on the answer of a call that yields (a program method, all?, count
# with a block), stored in a local or returned from a method: the value's own
# kind renders. The spliced call was taken for a route handing a String's
# handle on, whatever its type, and the Integer read as a handle was "" (a
# Float or an Array there did not compile).
def m = yield(1)
s = m { |x| x + 1 }.to_s
p s
t = [5].all? { |x| x > 0 }.to_s
p t
u = [5, 6].count { |x| x > 5 }.to_s
p u
def g = m { |x| x * 3 }.to_s
p g
x = m { |x| 1.5 }.to_s
p x
y = m { |x| [1] }.to_s
p y
z = m { |x| x }.to_s
z << "!"
p z
w = m { |x| "s" }.to_s
p w
