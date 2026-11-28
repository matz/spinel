# A singleton alias of `new` taken before the class redefines new: the alias
# is the allocation, as it stood where the alias appeared (activesupport's
# TimeZone: `class << self; alias_method :create, :new; def new(name) = self[name]`,
# and `self[]` builds a zone with a bare `create`)
class Zone
  class << self
    alias_method :create, :new
    def new(name) = self[name]
    def [](name) = all[name] ||= create(name)
    def all = (@all ||= {})
  end
  def initialize(name) = @name = name
  attr_reader :name
end
p Zone.new("Tokyo").name
p Zone.new("Tokyo").equal?(Zone["Tokyo"])
p Zone.create("Osaka").name
p Zone.all.keys
