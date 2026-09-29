# A class hook named by its constant (def T.method_missing) answers its names;
# its super for the rest is NoMethodError.
class T
  def T.method_missing(name, *args) = name == :hi ? :hello : super
end
p T.hi
begin
  T.zork
rescue NoMethodError => e
  p e.class
end
