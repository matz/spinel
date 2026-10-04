# A bare `super` in a forwarding `self.new(*args, **options, &block)` builds
# the object from the arguments it holds; a default of initialize that
# reads an earlier parameter -- activesupport's DeprecatedConstantProxy,
# `message: "#{old_const} is deprecated! ..."` -- has to see that value.
# The default was spelled into the constructor call naming initialize's
# own parameter, which the wrapper has no local for, and the C did not
# compile.
class Dep
  def self.new(*args, **options, &block)
    object = args.first
    return object unless object
    super
  end

  def initialize(old_const, new_const, tag = old_const.downcase, message: "#{old_const} is deprecated! Use #{new_const} instead.")
    @tag = tag
    @msg = message
  end
  attr_reader :tag, :msg
end

d = Dep.new("A", "B")
p d.tag, d.msg
d = Dep.new("A", "B", "t", message: "custom")
p d.tag, d.msg
p Dep.new(nil, "B")
