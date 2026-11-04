# `klass.class_eval { default_helper_module! }` on an inherited hook's class
# value: a bare call in the block is a call on that class, as the block's
# self is the class, so it runs as `klass.default_helper_module!`, beside the
# ivar writes such a block may hold. Before, a block with a call was left as
# it was and refused.
class Base
  def self._helpers = @_helpers
  def self._helpers=(v); @_helpers = v; end
  def self.default_helper_module! = (@_helpers = [name.downcase + "_helper"])
  def self.inherited(klass)
    super
    klass._helpers = nil
    klass.class_eval { default_helper_module! } unless klass.name.nil?
    klass.class_eval do
      @depth = (@depth || 0) + 1
      tag(:inherited)
    end
  end
  def self.tag(t) = (@tags ||= []) << t
  def self.tags = @tags
  def self.depth = @depth
end
class Posts < Base; end
class Drafts < Posts; end
p Posts._helpers, Drafts._helpers, Base._helpers
p Posts.depth, Drafts.depth, Posts.tags, Drafts.tags, Base.tags
