# `klass.instance_variable_defined?(:@x)` on a class value, as
# instance_variable_set / _get on one already go: through the class's own
# accessor, answering true or false. An `inherited` hook asks it of each
# subclass before giving the ivar a default.
class Base
  def self.abstract! = @abstract = true
  def self.abstract? = @abstract
  def self.inherited(klass)
    p klass.instance_variable_defined?(:@abstract)
    unless klass.instance_variable_defined?(:@abstract)
      klass.instance_variable_set(:@abstract, false)
    end
    super
  end
end
class App < Base; abstract!; end
class Posts < App; end
p Base.abstract?, App.abstract?, Posts.abstract?
p Base.instance_variable_defined?(:@abstract), App.instance_variable_defined?(:@abstract)
p Posts.instance_variable_get(:@abstract)
