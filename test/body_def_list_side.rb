# A class-side list (`K.singleton_methods`) names no instance method, so with a respond_to_missing?
# in the program it leaves the versions of a method a body makes on the instance side alone: a call
# before the body runs raises NoMethodError, after it answers.
class K
  def self.setup = K.class_eval { def m = :k }
end
class Other
  def respond_to_missing?(name, include_all = false) = false
end
p K.singleton_methods
k = K.new
begin; p k.m; rescue NoMethodError => e; p e.class; end
K.setup
p k.m
