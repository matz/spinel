# `self.a ||= v` (and `self.a += v`) in a module's `def self.included(base);
# base.class_eval do ... end; end`, with `a` and `a=` class methods the hook
# defines: spliced into the includer's body, where self is the class, the
# statement reads and writes the includer, as `self.a = self.a || v` does.
# The statement is still inside the class_eval block when the lowering runs,
# and a class_eval block was taken for one whose self is something else, so
# the statement was left for a dispatch that had no method; self in a
# class_eval block is the class the block is run on.
class Config
  attr_accessor :token, :strategy, :count
end
module Protection
  def self.included(base); base.class_eval do
    def self.token(*, **, &) = config.token(*, **, &); def self.token=(v); config.token = v; end
    def token(*, **, &) = config.token(*, **, &)
    def self.count = config.count || 0
    def self.count=(v); config.count = v; end
    self.token ||= :authenticity_token
    self.count += 1
  end; end
end
class Controller
  def self.config = @config ||= Config.new
  def config = self.class.config
  include Protection
end
class Admin
  def self.config = @config ||= Config.new.tap { |c| c.token = :admin_token }
  def config = self.class.config
  include Protection
  include Protection
end
p Controller.token, Controller.new.token, Controller.count
p Admin.token, Admin.count
