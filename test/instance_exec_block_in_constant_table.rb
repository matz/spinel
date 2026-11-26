# A block a module method keeps in a constant Hash, run by instance_exec on
# another object: its variable-like calls reach that object's
# method_missing, and its plain calls the object's methods.
class Ctx
  def initialize(args) = (@args = args)
  def title = "t"

  def method_missing(name, *args)
    return @args[name] if @args.key?(name)
    raise NoMethodError, "undefined method '#{name}'"
  end
end

module App
  PAGES = {}

  def self.page(name, &body)
    PAGES[name] = body
  end
end

App.page :post do
  id
end

App.page :both do
  [title, id]
end

p Ctx.new({ id: 1 }).instance_exec(&App::PAGES[:post])
p Ctx.new({ id: 2 }).instance_exec(&App::PAGES[:both])
