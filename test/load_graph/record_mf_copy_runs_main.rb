# Where a module_function copy's def may first run (--dump-load-record,
# record_mf_copy_runs_main.rb.load-record, "runs="): a copy runs the instance
# method as it stood at the copy, though no object of a module of functions
# may exist. A copy, bare or named (`module_function :c`), reaches the def it
# copies, followed to the original an alias took (Y.ya copies `alias ya yb`).
# So the bodies behind U.b, V.d and Y.ya, called by the statements after them,
# are not "never run", which a call in them would read as proof that its
# window is the last statement. A module whose functions nothing calls (W)
# stays "never", and so does a def after the copy that none took (Y's yb).
module U
  module_function
  def a(x) = x + 1
  def b(x) = a(x) * 2
end
puts U.b(3)
def U.a(x) = 100
puts U.b(3)
module V
  def c = 1
  def d = c + 1
  module_function :c, :d
end
puts V.d
def V.c = 50
puts V.d
module W
  module_function
  def e = 1
  def e = 2
end
module Y
  def yb(x) = yc(x) * 2
  def yc(x) = x + 1
  alias ya yb
  module_function :ya, :yc
end
puts Y.ya(3)
module Y
  def yb(x) = 0
end
