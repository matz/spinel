# A class_variable_get / _set / _defined? whose literal name is no class
# variable name (`:"@@x-y"`, `:x`) raises CRuby's NameError when the call
# runs, naming the class and the text, and declares nothing: the name's C
# identifier would not even have compiled.
class A
  @@ok = 1
  begin
    class_variable_set(:"@@x-y", 1)
  rescue NameError => e
    p e.message, e.name, e.receiver
  end
  def self.ok = @@ok
  def self.probe(flag) = flag ? class_variable_defined?(:"@@x-y") : class_variable_defined?(:@@ok)
end
p A.ok
begin
  A.class_variable_get(:"@@bad name")
rescue NameError => e
  p e.message
end
begin
  A.class_variable_set("x", 2)
rescue NameError => e
  p e.message
end
p A.probe(false)
begin
  A.probe(true)
rescue NameError => e
  p e.message
end
p A.class_variable_defined?(:@@ok), A.class_variable_get(:@@ok)
