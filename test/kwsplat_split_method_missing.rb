# In a program with a method_missing, a `**` call on a boxed receiver is split
# as in any other: the branch every builtin's count refuses may reach a
# method_missing, so it answers a boxed value
class M2
  def method_missing(n, *a, **kw) = [:mm, n, a, kw]
  def respond_to_missing?(*) = true
end
e = {}
x = [[3], [3, 1, 2]].sample
p x.first(1, **e)
y = [[4, 5], [4]].sample
r = y.first(1, **e)
p r
