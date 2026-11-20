# The value form of class_eval on a constant path: `Outer::Inner.class_eval { }`
# and `::Base.class_eval { }` run the block with that class as self, as the
# bare-constant form does. A large application program's load hooks run as
# `::ActionController::Base.class_eval do ... end` from inside other modules.
module AC
  class Base
    def self.greet = "hi from #{self}"
    def self.paths = (@paths ||= [])
    attr_reader :x
    def initialize = @x = 7
  end
end

views = ["v"]
p AC::Base.class_eval { greet }
::AC::Base.class_eval { paths.concat(views) }
p AC::Base.paths

module Outer
  def self.go(more)
    ::AC::Base.class_eval { paths.concat(more) }
    AC::Base.class_eval { self }
  end
end
p Outer.go(["w"])
p AC::Base.paths
p AC::Base.class_eval { paths.size + 1 }
p AC::Base.class_exec(3) { |n| paths.size + n }
