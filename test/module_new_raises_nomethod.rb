# `M.new` on a module is CRuby's NoMethodError, with any arguments: a module
# has no constructor, and its missing `new` is not an arity error of one.
module M
  def self.x=(v); @x = v; end
  def self.x; @x; end
end
begin
  M.new(1)
rescue NoMethodError => e
  p e.message
end
begin
  M.new
rescue NoMethodError => e
  p e.message
end
M.x = 3
p M.x
