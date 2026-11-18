# A String handle obtained from a mixed Array still dispatches to a
# program's String#<=> when the receiver is boxed.
class String
  def <=>(other)
    42
  end
end

def ordering(a, b)
  a <=> b
end

items = [+"left", :other]
v = items[0]
p ordering(v, "right")
p ordering([:a, 1][0], :b)
p ordering([1, "x"][0], 3)
