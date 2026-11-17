# A method that forwards its block to a boxed receiver's instance_eval and
# then to a K's: the literal runs on K, the class the later forward names.
class K
  def touch = :k
end
class L
  def touch = :l
end
class Runner
  def initialize = @h = { x: K.new, y: L.new }
  def run(&b)
    @h[ARGV[0] ? :y : :x].instance_eval(&b)
    K.new.instance_eval(&b)
  end
end
p Runner.new.run { touch }
