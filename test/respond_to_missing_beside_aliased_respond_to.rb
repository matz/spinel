# A respond_to? the program aliases answers itself: respond_to_missing? is
# not asked behind it.
class R
  def respond_to_missing?(n, p = false) = n == :dyn
  def rt2(n, p = false) = n == :only
  alias respond_to? rt2
end
p R.new.respond_to?(:dyn), R.new.respond_to?(:only)
