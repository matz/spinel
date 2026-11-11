# A receiverless method(name) with a name known only at run time is self.method(name)
class M
  def foo = 1
  def bar = 2
  def fetch(key)
    m = method(key)
    [m.name, m.call]
  end
end
key = ARGV.fetch(0, "b") + "ar"
p M.new.fetch(key)
p M.new.fetch(key.sub("bar", "foo").to_sym)
