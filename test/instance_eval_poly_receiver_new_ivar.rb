# An instance_eval/exec block on a receiver of several classes, whose body
# only reads and writes ivars, runs on every class the receiver can be. The
# classes were guessed from which ones already own the block's ivars, so a
# receiver whose class did not (the enclosing class itself, or one that
# never wrote the name) raised NoMethodError for instance_eval.
class State
  def initialize = (@t = 0; @v = 1)
end

class Other
  def initialize = (@v = 2.5)
end

class Poke
  def go(st) = st.instance_eval { @t = 1 }
  def mark(st) = st.instance_eval { @w ||= :x }
  def copy(st) = st.instance_exec(3) { |n| @u = [@v, n] }
end

k = Poke.new
p [State.new, k].map { |o| Poke.new.go(o) }
p k.instance_variables
s = State.new
o = Other.new
p [s, o].map { |r| Poke.new.mark(r) }
p o.instance_variables
p [s, o].map { |r| Poke.new.copy(r) }
p s.instance_variables, o.instance_variable_get(:@u)
