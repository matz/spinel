# A Method of an Integer or a String array's [], []= or push is Array's,
# also read back from a container.
names = ["a", "b"]
ws = [names.method(:push), names.method(:[]), names.method(:[]=), 1]
p ws[0].owner, ws[1].owner, ws[2].owner
ws[0].call("c")
p names
ram = [0, 0, 0]
m = ram.method(:push)
p m.owner
st = [m, ram.method(:[]), ram.method(:[]=), 2]
p st[0].owner, st[1].owner, st[2].owner
p ram.method(:[]=).arity
