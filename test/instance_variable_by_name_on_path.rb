# An ivar of a class or module named by a path, set and read by name: a
# hook of Outer::Reg that writes the module's own ivar, and
# Wrap::Inner.instance_variable_set from the top level.
module PvOuter
  module Reg
    @count = 0
    module CM
      def kind = "cm"
    end
    def self.included(base)
      @count += 1
      @names ||= []
      @names << base.name
      @last = base.name if @count > 1
      base.extend(CM)
    end
    def self.count = @count
    def self.names = @names
    def self.last = @last
  end
end
class PvA; include PvOuter::Reg; end
class PvB; include PvOuter::Reg; end
p PvOuter::Reg.count, PvOuter::Reg.names, PvOuter::Reg.last, PvA.kind

module PvWrap
  class Inner
    @w = 2
    def self.w = @w
  end
end
PvWrap::Inner.instance_variable_set(:@w, 9)
p PvWrap::Inner.w, PvWrap::Inner.instance_variable_get(:@w), PvWrap::Inner.instance_variable_defined?(:@w), PvWrap::Inner.instance_variable_defined?(:@none)
