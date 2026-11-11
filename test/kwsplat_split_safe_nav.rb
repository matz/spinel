# A `&.` call tests its receiver first and evaluates no operand when it is nil.
N = [0]
def nx = (N[0] += 1)
def kwm = (N[0] += 10; {})
def mk(f) = f ? [] : nil
x = mk(false)
p x&.push(nx, **kwm), N
y = mk(true)
p y&.push(nx, **kwm), N
e = {}
n = {a: 1}
p mk(false)&.push(1, **e), mk(true)&.push(2, **e), mk(true)&.push(3, **n)
p mk(false)&.sum(**e), [1, 2]&.sum(**e)
s = mk(true)&.push(4, **e)
p s.class
