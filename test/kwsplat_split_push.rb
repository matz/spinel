# push on an Array: the empty `**` operand adds nothing, the non-empty one adds one Hash.
e = {}
n = {a: 1}
z = nil
a = [1]
p a.push(**e)
p a.push(**n)
p a.push(2, **e)
p a.push(**z)
p a.push(**{})
p a.push(**{b: 2})
x = a.push(3, **e)
p x.equal?(a), x.size
a.push(**e)
p a.size
p [].push(**e).size, [].push(**n).size
