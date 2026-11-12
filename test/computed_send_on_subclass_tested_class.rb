# `klass < Base ? klass : nil` holds only Base's subclasses, as Class
# objects: a `new` on it builds one of them, and a computed send on the
# instance dispatches over their methods alone. A class outside Base, with
# a constructor of the same shape, is neither built nor an arm.

class Base
  def initialize(req) = @req = req
  def base_name = "base #{@req}"
end

class Users < Base
  def list = "Users#list #{@req}"
end

class Posts < Base
  def list = "Posts#list #{@req}"
  def show = "Posts#show #{@req}"
end

class Other
  def initialize(req) = @req = req
  def list = "Other#list #{@req}"
  def other_only = "other"
end

Other.new(0).other_only if ARGV.size > 99

def find(name)
  klass = Object.const_get(name)
  klass.is_a?(Class) && klass < Base ? klass : nil
rescue NameError
  nil
end

def dispatch(name, action, req)
  klass = find(name)
  return "no controller #{name}" if klass.nil?
  obj = klass.new(req)
  obj.public_send(action.to_sym)
rescue NoMethodError => e
  "NoMethodError #{e.name}"
end

puts dispatch("Users", "list", 1)
puts dispatch("Posts", "show", 2)
puts dispatch("Posts", "base_name", 3)
puts dispatch("Users", "show", 4)
puts dispatch("Other", "list", 5)
puts dispatch("Base", "base_name", 6)
puts dispatch("Missing", "list", 7)
puts dispatch("String", "size", 8)

# `<=` keeps the class itself
def find_le(name)
  klass = Object.const_get(name)
  klass.is_a?(Class) && klass <= Base ? klass : nil
end
k = find_le("Base")
puts k ? k.new(9).base_name : "nil"
k = find_le("Other")
puts k ? k.new(10).list : "nil"
