# A dup before a reopen of initialize_copy that runs only under a condition (here not taken) runs the version before it: the conditional def's first arm makes the name where the arm runs, so the load record does not prove no hook.
class K
  attr_accessor :t
  def initialize_copy(o) = @t = :a
end
k = K.new
p k.dup.t
if ARGV.size > 5
  class K
    def initialize_copy(o) = @t = :b
  end
end
p k.dup.t
if ARGV.empty?
  class K
    def initialize_copy(o) = @t = :c
  end
end
p k.dup.t
