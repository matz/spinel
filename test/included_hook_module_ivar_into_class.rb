# A hook reads the module's own ivar into a local and hands the value to
# the including class, which also sets an ivar of its own in the
# class_eval block.
module IvCfg
  @default = :strict
  def self.included(base)
    mode = @default
    base.class_eval do
      @mode = :own
    end
    base.instance_variable_set(:@from_mod, mode)
  end
end
class IvCfgHost
  include IvCfg
  def self.mode = @mode
  def self.from_mod = @from_mod
end
p IvCfgHost.mode, IvCfgHost.from_mod
