# A call with an explicit receiver that runs before every def of its name
# on Object, Kernel, Numeric or NilClass raises CRuby's undefined-method
# NoMethodError, naming the receiver; from the def on it runs the method,
# or raises the private-method error for a private one. A module the root
# includes counts from its include: a private tap of an included module
# is private on every receiver, and one included after the call leaves
# the call to the builtin tap.

class Foo; end

begin; p(nil.obj_priv); rescue NoMethodError => e; puts e.message; end
begin; p(1.5.obj_priv); rescue NoMethodError => e; puts e.message; end
begin; p(nil.kern_priv); rescue NoMethodError => e; puts e.message; end
begin; p(1.kern_priv); rescue NoMethodError => e; puts e.message; end
begin; p(nil.kern_pub); rescue NoMethodError => e; puts e.message; end
begin; p(2.num_pub); rescue NoMethodError => e; puts e.message; end
begin; p(nil.nil_pub); rescue NoMethodError => e; puts e.message; end
begin; p(Foo.new.obj_pub); rescue NoMethodError => e; puts e.message; end

class Object
  private
  def obj_priv; :obj_priv; end
end
module Kernel
  private
  def kern_priv; :kern_priv; end
end
module Kernel
  def kern_pub; :kern_pub; end
end
class Numeric
  def num_pub; :num_pub; end
end
class NilClass
  def nil_pub; :nil_pub; end
end
class Object
  def obj_pub; :obj_pub; end
end

begin; p(nil.obj_priv); rescue NoMethodError => e; puts e.message; end
begin; p(1.5.obj_priv); rescue NoMethodError => e; puts e.message; end
begin; p(nil.kern_priv); rescue NoMethodError => e; puts e.message; end
begin; p(1.kern_priv); rescue NoMethodError => e; puts e.message; end
begin; p(nil.kern_pub); rescue NoMethodError => e; puts e.message; end
begin; p(2.num_pub); rescue NoMethodError => e; puts e.message; end
begin; p(nil.nil_pub); rescue NoMethodError => e; puts e.message; end
begin; p(Foo.new.obj_pub); rescue NoMethodError => e; puts e.message; end

module Late
  private def then; :late_then; end
end
begin; p(nil.then { |x| x.inspect }); rescue NoMethodError => e; puts e.message; end
class Object; include Late; end
begin; p(nil.then { |x| x.inspect }); rescue NoMethodError => e; puts e.message; end
begin; p(1.then { |x| x }); rescue NoMethodError => e; puts e.message; end
