# `alias_method :plus_without, :+` then `alias_method :+, :plus_with` in a
# reopened builtin: the first alias captures the builtin `+` as it was, so
# plus_without keeps adding after `+` names the program's method. Resolved
# through the later alias, plus_with called itself without end; a Time
# chain named a C function that was never written. A name aliased twice is
# the last alias, and an alias names what its target was where it appeared,
# not a def that comes after it.
class Integer
  def plus_tenfold(o) = plus_without(o) * 10
  alias_method :plus_without, :+
  alias_method :+, :plus_tenfold

  alias_method :old_succ, :succ
  def succ = old_succ + 100

  def minus_one(o) = minus_without(o).pred
  def minus_two(o) = minus_without_one(o).pred.pred
  alias_method :minus_without, :-
  alias_method :-, :minus_one
  alias_method :minus_without_one, :-
  alias_method :-, :minus_two
end

p 1 + 2
p 1.plus_without(2)
p 5.succ
p 5.old_succ
p 10 - 3

class Time
  def plus_with_double(other) = plus_without_double(other * 2)
  alias_method :plus_without_double, :+
  alias_method :+, :plus_with_double

  def compare_with_coercion(other)
    other.is_a?(Time) ? compare_without_coercion(other) : nil
  end
  alias_method :compare_without_coercion, :<=>
  alias_method :<=>, :compare_with_coercion
end

t = Time.at(0).utc
p (t + 5).to_i, t.plus_without_double(5).to_i
p t <=> Time.at(1), t <=> 3
