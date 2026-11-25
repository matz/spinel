# Class.new and Module.new without a block, evaluated more than once: a class
# (a module) of its own each time, and the superclass's inherited runs at
# each evaluation, with the class it makes, before the call answers it.
class Plugin
  @made = []
  def self.inherited(sub)
    @made << sub
    super
  end
  def self.made = @made
  def kind = :plugin
end

def plugin = Class.new(Plugin)
def bare = Class.new
def mod = Module.new

ps = []
3.times { ps << plugin }
p Plugin.made.size, Plugin.made == ps, ps.uniq.size
p ps.map { |k| k.new.kind }.uniq, ps.all? { |k| k.superclass == Plugin }
c = bare
d = bare
p c == d, c.superclass, c.new.class == c, d.new.class == d
m = mod
n = mod
p m == n, m.class, m.is_a?(Module), m.instance_of?(Class)
Late = n
p n.name, m.name
