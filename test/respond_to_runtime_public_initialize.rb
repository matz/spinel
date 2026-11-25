# A respond_to? of a name the run gives answers an initialize the class made
# public, with `public :initialize` and with no def of its own, as the same
# call with a literal name does.
class Pub
  public :initialize
end
class PubDef
  def initialize; end
  public :initialize
end
class Priv
  def initialize; end
end
names = [:initialize, :zz]
m = names[ARGV.size]
p [Pub.new.respond_to?(:initialize), Pub.new.respond_to?(m)]
p [PubDef.new.respond_to?(:initialize), PubDef.new.respond_to?(m)]
p [Priv.new.respond_to?(:initialize), Priv.new.respond_to?(m), Priv.new.respond_to?(m, true)]
