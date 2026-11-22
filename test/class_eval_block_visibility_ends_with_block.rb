# A visibility word in a class_eval block of the body's own class ends with
# the block: a def after the block is public again.
class A
  class_eval do
    private
    def inside = 1
  end
  def outside = 2
end
a = A.new
p((a.inside rescue :private), (a.outside rescue :private))
