# A `class Module` reopening's method is every class's and module's, so a
# class or module body may call it as a macro: one that writes its methods
# with a String module_eval is expanded in the body, as a method of a module
# the class extends is. The call used to be skipped as a declaration, and
# the methods it writes were missing (NoMethodError where CRuby answers).
# A rest parameter takes the names the call lists (Active Support's
# `def delegate(*methods, to:)` shape).
class Module
  def forward(*names, to:)
    module_eval("def #{names[0]} = #{to}.#{names[0]}")
    module_eval("def #{names[1]} = #{to}.#{names[1]}") if names.size > 1
  end

  def flag(name)
    module_eval("def #{name}? = true")
  end
end

class Config
  def host = "example.com"
  def port = 8080
end

class Server
  def config = Config.new
  forward :host, :port, to: :config
  flag :ready
end

module Named
  def label = "named"
  forward :size, to: :label
end

class Box
  include Named
end

s = Server.new
p s.host, s.port, s.ready?
p Box.new.size
