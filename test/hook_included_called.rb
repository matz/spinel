# A hook that cannot stand in the class body runs as the method it is: one
# that calls super (Module#included answers nil), one that returns early,
# one defined in the module's `class << self`, and one defined under a
# run-time condition that does not hold, which leaves the core's no-op.
module WithSuper
  def self.included(b)
    r = super
    puts "super #{b} #{r.inspect}"
  end
end
class SuperA; include WithSuper; end

module Early
  def self.included(b)
    return puts("early #{b}") if b.name.start_with?("E")
    puts "late #{b}"
  end
end
class EarlyA; include Early; end
class LaterB; include Early; end

module Sclass
  class << self
    def included(b) = puts("sclass #{b}")
  end
end
class SclassA; include Sclass; end

module Cond
  unless ARGV.empty?
    def self.included(b) = puts("cond #{b}")
  end
end
class CondA; include Cond; end
puts "done"
