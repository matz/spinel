# A Float reopening's method of a name the numeric rows type (clamp with
# mixed bounds or a Range, round with keywords) answers the call: the
# call is typed from the reopening's method the emitter calls, not from
# the builtin's boxed result.
class Float
  def clamp(*a) = [:clamp, a.size]
  def round(*a, **k) = [:round, k.keys]
end
p 2.5.clamp(1, 2)
p 2.5.clamp(1.0, 2.0)
p 2.5.clamp(1..2)
p 2.5.round(half: :even)
p 3.clamp(1, 2)
p 3.clamp(1..2)
x = [1.5, 2].sum
p x.clamp(1, 2)
