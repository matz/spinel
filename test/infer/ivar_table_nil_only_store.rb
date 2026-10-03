# An ivar table of int arrays that also stores a value which only ever is
# nil: a local, a parameter, another method's answer. Such a value is nil
# through the fixpoint and is declared boxed after it. The table pass, run
# once more after the fixpoint, must not take that for a boxed row and give
# the table up: each slot below stays a table of int arrays.
class ByLocal
  def initialize
    @loc = [[1, 2], [3]]
  end
  def clear(i)
    x = nil
    @loc[i] = x
    @loc[i]
  end
  def first(i) = @loc[i][0]
end
a = ByLocal.new
p a.first(0), a.first(1)
p a.clear(0)
p a.first(1)

class ByParam
  def initialize
    @par = [[4, 5], [6]]
  end
  def put(i, row)
    @par[i] = row
    @par[i]
  end
  def first(i) = @par[i][0]
end
b = ByParam.new
p b.first(0), b.first(1)
p b.put(0, nil)
p b.first(1)

class ByCall
  def initialize
    @ret = [[7, 8], [9]]
  end
  def put(i)
    @ret[i] = none
    @ret[i]
  end
  def none
    x = nil
    x
  end
  def first(i) = @ret[i][0]
end
c = ByCall.new
p c.first(0), c.first(1)
p c.put(0)
p c.first(1)
