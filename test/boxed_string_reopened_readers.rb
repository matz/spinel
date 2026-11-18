# A boxed String still dispatches its own reopened length and predicate methods.
class Other
  def length = 10
  def size = 11
  def empty? = false
  def count = 12
  def upcase = "other"
  def split = ["other"]
end

class String
  def length = 42
  def size = 43
  def empty? = true
  def count = 44
  def upcase = "mine"
  def split = ["mine"]
end

def passes(v)
  [v.length, v.size, v.empty?, v.count, v.upcase, v.split]
end
p passes([+"x", Other.new][0])
p passes(Other.new)
