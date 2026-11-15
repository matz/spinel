# A module whose methods use instance variables, included at the top level:
# every object answers them and keeps the variables on itself, main too.
module Cnt
  def bump = (@n = (@n || 0) + 1)
  def seen = @n
end
include Cnt
p bump, bump, seen
class K; end
k = K.new
p k.seen, k.bump, k.seen
p 5.seen
