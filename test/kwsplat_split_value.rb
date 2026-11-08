# The value of the call, its identity and its class are the callee's, with and without the split.
e = {}
n = {b: 2}
a = [1]
x = a.push(2, **e)
p x.equal?(a), x.class
y = [1, 2].sum(**e)
p y + 1
if [5].sum(**e) > 0 then p :pos else p :neg end
p "#{[1, 2].sum(**e)}-#{[1].push(**e).size}"
def m(e) = [3, 4].sum(**e)
p m(e)
def k(a, e) = a.push(**e)
p k([], e).size, k([], n).size
z = [{a: 1}].fetch(0, **e)
p z[:a]
q = [1, 2].first(**e)
p q * 10
