# A bare super passes the current value of the method's rest parameter: a body
# that assigns the rest before the super hands on the new array, as CRuby does,
# for a super into an Array subclass's builtin method.

class Reset < Array
  def push(*a)
    a = [9]
    super
  end
end
rs = Reset.new
rs.push(1, 2)
p rs
