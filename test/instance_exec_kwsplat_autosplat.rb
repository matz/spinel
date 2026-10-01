# instance_exec spreads a lone Array across a block's parameters only when
# it passes no keywords. A `**h` it passes is dropped when empty, so the
# spread then happens, but a non-empty one turns it off as any keywords do:
# `instance_exec([1, 2], **h)` into `|a, b, **kw|` binds a = [1, 2].
# Whether h is empty is known only at run time.

h = {z: 2}
e = {}
o = Object.new

o.instance_exec([1, 2], **h) { |a, b, **kw| p [a, b, kw] }
o.instance_exec([1, 2], **e) { |a, b, **kw| p [a, b, kw] }

# two splats: any non-empty one turns the spread off
o.instance_exec([1, 2], **e, **h) { |a, b, **kw| p [a, b, kw] }
o.instance_exec([1, 2], **e, **e) { |a, b, **kw| p [a, b, kw] }

# the Array itself splatted in
o.instance_exec(*[[1, 2]], **h) { |a, b, **kw| p [a, b, kw] }
o.instance_exec(*[[1, 2]], **e) { |a, b, **kw| p [a, b, kw] }

# optionals, a rest and a keyword
o.instance_exec([1, 2], **h) { |a, b = 5, *r, z: 0| p [a, b, r, z] }
o.instance_exec([1, 2], **e) { |a, b = 5, *r, z: 0| p [a, b, r, z] }

# a boxed Array
x = [[3, 4], "s"][0]
o.instance_exec(x, **h) { |a, b, **kw| p [a, b, kw] }
o.instance_exec(x, **e) { |a, b, **kw| p [a, b, kw] }

# keywords alone
o.instance_exec(**h) { |a, b, **kw| p [a, b, kw] }

# each value runs once, in source order
$l = []
def lg(v) = ($l << v; v)
o.instance_exec(lg([1, 2]), **lg(h)) { |a, b, **kw| p [a, b, kw] }
o.instance_exec(lg([1, 2]), **lg(e)) { |a, b, **kw| p [a, b, kw] }
p $l.size
