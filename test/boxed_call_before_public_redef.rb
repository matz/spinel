# A boxed call made before a class redefines a private method public still
# raises NoMethodError, as the typed call does: the refusal's name set holds
# a name whose visibility differs between points of the load, not only the
# names the end-of-load table lists as private.

class K
  private def m = 1
end
k = K.new
xs = [k, "s"]
begin; p k.m; rescue NoMethodError => e; p e.class; end
xs.each { |x| begin; p x.m; rescue NoMethodError => e; p e.class; end }
class K
  def m = 2
end
p k.m
xs.each { |x| begin; p x.m; rescue NoMethodError => e; p e.class; end }
