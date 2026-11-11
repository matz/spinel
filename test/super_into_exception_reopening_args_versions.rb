# A super with arguments in a builtin exception's reopening reaches the version
# of the base reopening's method that is live at the super (a new version
# takes an optional argument): each call answers its own version.
class StandardError
  def tag(a) = "se1-#{a}"
end
class TypeError
  def tag(a) = "TE:" + super
end
def go(x) = puts(TypeError.new("t").tag(x))
go(1)
class StandardError
  def tag(a, b = :d) = "se2-#{a}-#{b}"
end
go(2)
