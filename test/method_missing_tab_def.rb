# a hook defined with a tab after def still gets Object's default for super
class T
  def	method_missing(name, *args) = super
end
begin
  T.new.zork
rescue NoMethodError => e
  p e.name
end
