# Array#push, append, unshift, insert and concat never call a block. With a
# literal block beside a `*splat`, the splat still spreads its elements.
class A; def foo = "A"; end
class B; def foo = "B"; end
b = [B.new, A.new]
i = [A.new]
i.push(*b) { }
p i.size
p i.map(&:foo)

n = [1]
r = [2, 3]
n.push(*r) { |x| x }
p n
n.unshift(*r) { }
p n
n.insert(1, *r) { }
p n
n.append(*r) do end
p n
q = n.push(*r) { }
p q.size

s = ["a"]
s.concat(*[["b"], ["c"]]) { }
p s

f = [1.5]
p f.push(*[2.5]) { }

m = [1, "x"]
m.push(*[:y]) { }
p m

x = [[1], "s"].first
x.push(*[2, 3]) { }
p x

def grow(a, *rest) = a.push(*rest) { }
p grow([0], 1, 2)
