# A Method captured on main after `extend M` is M's, which stands ahead of a top-level def of the name.
def f = "top"
module M
  def f = "M"
end
extend M
m = method(:f)
p m.call
