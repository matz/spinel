# A method_added hook installed by its name (define_singleton_method, a
# keyword alias) hears of every def, so no def is left out.
class Base
  define_singleton_method(:method_added) { |m| puts "Base added #{m}" }
  def unused(xs) = xs.sum
  def last_one = :kept
end
class Other
  class << self
    def my_hook(m) = puts("Other added #{m}")
    alias method_added my_hook
  end
  def spare(xs) = xs.size
  def last_one = :kept
end
puts "done"
