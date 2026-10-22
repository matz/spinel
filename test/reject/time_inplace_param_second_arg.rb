# The Time parameter handed on second, after a plain local, still reaches the
# lent slot of `stamp`: an initialize cannot lend it, so it is refused.
# spinel: reject-time-inplace: whose parameters are passed by value
class Stamp
  def initialize(t)
    x = Time.utc(2026, 10, 15, 1, 0, 0)
    stamp(x, t)
  end

  def stamp(a, b)
    a.localtime(3600)
    b.localtime(3600)
  end
end

a = Time.utc(2026, 10, 15, 1, 0, 0)
Stamp.new(a)
p a.utc_offset
