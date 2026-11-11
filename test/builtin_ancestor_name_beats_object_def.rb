# A name a builtin class gets from an ancestor below Object (Comparable,
# Numeric, Enumerable) answers ahead of the program's Object method of the
# same name, for the class's own receivers: Time and Symbol included.
class Object
  def between?(*a) = :mine
  def clamp(*a) = :mine
  def abs2(*a) = :mine
  def sum(*a) = :mine
  def min_by(*a) = :mine
  def mine_only(*a) = :mine
end

t0 = Time.at(0)
t1 = Time.at(1)
p t1.between?(t0, t1)
p t1.clamp(t0, t0) == t0
p 1.between?(0, 2)
p 1.5.clamp(0, 1)
p :b.clamp(:a, :c)
p "b".between?("a", "c")
p 3.abs2
p [1, 2].sum
p (1..3).min_by { |x| -x }
p t0.mine_only
p nil.mine_only
p 1.mine_only
