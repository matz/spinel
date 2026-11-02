# The included and extended hooks a mixin fires are the ones the module has
# where the mixin runs: a hook added after the first include, redefined or
# removed between two includes, or redefined in the module's body ahead of
# an include there, answers per include, not with the first def found.
module Late; end
class LateA
  include Late
end
module Late
  def self.included(b) = puts("late #{b}")
end
class LateB
  include Late
end

module Redef
  def self.included(b) = puts("v1 #{b}")
end
class RedefA; include Redef; end
module Redef
  def self.included(b) = puts("v2 #{b}")
end
class RedefB; include Redef; end

module Gone
  def self.included(b) = puts("gone #{b}")
end
class GoneA; include Gone; end
class << Gone
  remove_method :included
end
class GoneB; include Gone; end

module Ext; end
class ExtA
  extend Ext
end
module Ext
  def self.extended(b) = puts("ext #{b}")
end
class ExtB
  extend Ext
end

module Same
  def self.included(b) = puts("same v1 #{b}")
end
class SameA; include Same; end
module Same
  def self.included(b) = puts("same v2 #{b}")
  class Inner; include Same; end
end

module Inside; end
class InsideA
  include Inside
  module ::Inside
    def self.included(b) = puts("inside #{b}")
  end
  include Inside
end
puts "done"
