# method_missing for a writer, an index assignment and a send of a
# writer's name
class Settings
  def method_missing(name, *args)
    puts "#{name} #{args.inspect}"
    :hook
  end

  def respond_to_missing?(name, include_private = false) = true
end

s = Settings.new
s.port
s << 2
s.send(:host)
s.port = 3000
s[:a] = 1
s.send(:host=, "x")

# an assignment answers the value it assigns; a send answers the hook's
x = (s.port = 3)
y = s.send(:port=, 4)
z = (s[:a, :b] = 5)
p x, y, z

# a class with a writer of its own
class Own
  def method_missing(name, *args) = :hook
  def port=(v)
    @port = v
  end
  def port = @port
end

o = Own.new
p((o.port = 9), o.port)

# net-ldap's Entry shape
class Entry
  def method_missing(sym, *args)
    p [sym, args]
  end
end

e = Entry.new
e.sn
e.sn = "Larsen"

# a writer nested in another's value
class Rec
  def method_missing(name, *args)
    p [name, args]
    :hook
  end
end

sink = Rec.new
source = Rec.new
r = (sink.port = source.port = 7)
p r
