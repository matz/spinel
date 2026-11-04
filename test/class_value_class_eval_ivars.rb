# `klass.class_eval do @x = v end` on a class value a method can only have as
# the class or a subclass (the `inherited` hook's parameter): self in the
# block is that class, so each `@x = v` and `@x` is its class-level ivar,
# written and read through the class's own accessors, as
# `klass.instance_variable_set(:@x, v)` already is. Before, the call was
# refused.
class Metal
  def self.controller_name = @controller_name ||= name.downcase
  def self.inherited(subclass)
    super
    subclass.class_eval do
      @controller_name = nil
      @depth = (@depth || 0) + 1
    end
  end
  def self.depth = @depth
end
class Posts < Metal; end
class Drafts < Posts; end
p Posts.controller_name, Metal.controller_name, Drafts.controller_name
p Metal.depth, Posts.depth, Drafts.depth
