# A new before a reopen of initialize that runs only under a condition (here not taken) runs the version before it, as a dup runs initialize_copy's (dup_before_conditional_initialize_copy_reopen).
class K
  attr_accessor :t
  def initialize = @t = :a
end
p K.new.t
if ARGV.size > 5
  class K
    def initialize = @t = :b
  end
end
p K.new.t
