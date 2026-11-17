# spinel: gc-minor
# spinel: gc-stress
# An element write whose receiver is a call's answer (`bx.a`) keeps that Array alive while an operand
# assigns the local the call read (`bx = nil`) and allocates: the local was the only holder of the Box.
class Box
  attr_accessor :a, :s

  def initialize
    @a = [1, 2, 3]
    @s = [+"p", +"q"]
  end
end

def store
  bx = Box.new
  keep = bx.a.size
  bx.a[1] = (bx = nil; [[1, 2], [3, 4], [5, 6], [7, 8], [9, 10]].size)
  keep
end
p store

def value
  bx = Box.new
  x = (bx.a[1] = (bx = nil; [[1, 2], [3, 4], [5, 6], [7, 8], [9, 10]].size))
  x
end
p value

def and_str
  bx = Box.new
  x = (bx.s[1] &&= (bx = nil; "#{[[1, 2], [3, 4], [5, 6]].size}z"))
  x
end
p and_str

def and_stmt
  bx = Box.new
  bx.a[1] &&= (bx = nil; [[1, 2], [3, 4], [5, 6], [7, 8], [9, 10]].size)
  1
end
p and_stmt

def splice
  bx = Box.new
  bx.a[0, 1] = (bx = nil; [[1, 2], [3, 4], [5, 6]].size; [7])
  1
end
p splice
